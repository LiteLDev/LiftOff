// Selftests of U1 block pickup (docs/SPEC.md §3.1), dx 100..199.

#include <algorithm>
#include <string>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/ChestBlockActor.h"
#include "mc/world/phys/AABB.h"

#include "feature/blockpickup/BlockPickup.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace bp    = lo::feature::blockpickup;
using bp::Refusal;
using carry::CarryData;

// ---- test hooks (inactive unless a test switches them on) ---------------------------------------
struct TestHookState {
    bool        veto       = false; ///< customCondition false
    bool        protect    = false; ///< protection false
    bool        script     = false; ///< beforePickup false
    bool        override   = false; ///< overrideChecks true
    std::string activeScript;       ///< written by beforePickup when set
    int         protectionCalls = 0;
};
TestHookState gHooks;

LO_REGISTER_PICKUP_HOOKS(carry::PickupHooks{
    .name            = "blockpickup.selftest",
    .overrideChecks  = [](carry::PickupTarget const&) { return gHooks.override; },
    .customCondition = [](carry::PickupTarget const&) { return !gHooks.veto; },
    .protection =
        [](carry::PickupTarget const&) {
            ++gHooks.protectionCalls;
            return !gHooks.protect;
        },
    .beforePickup =
        [](carry::PickupTarget const&, CarryData& data) {
            if (gHooks.script) return false;
            if (!gHooks.activeScript.empty()) data.activeScript = gHooks.activeScript;
            return true;
        },
});

/// Restores the carry config (and the compiled lists), the test hooks and the player state at the
/// end of a test, also when it aborts.
struct Restore {
    Player&                 player;
    carry::CarryConfig      saved = lo::config::get<carry::CarryConfig>();
    explicit Restore(Player& p) : player(p) { gHooks = {}; }
    ~Restore() {
        lo::config::mut<carry::CarryConfig>() = saved;
        carry::rebuildLists();
        gHooks = {};
        carry::clear(player);
        carry::removeSlowness(player);
        carry::resetKeyPressed(player);
        player.setSneaking(false);
        player.setPlayerGameType(GameType::Survival);
    }
};

carry::CarryConfig::Settings& settings() { return lo::config::mut<carry::CarryConfig>().settings; }

SimulatedPlayer& simOf(Context& ctx) { return static_cast<SimulatedPlayer&>(ctx.player()); }
BlockSource&     regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

lo::core::GlobalPos offset(lo::core::GlobalPos p, int dx, int dy = 0, int dz = 0) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}

int itemsNear(BlockSource& region, BlockPos const& pos) {
    AABB box{
        Vec3{static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z)},
        Vec3{static_cast<float>(pos.x + 1), static_cast<float>(pos.y + 1), static_cast<float>(pos.z + 1)},
    };
    return static_cast<int>(region.getEntities(box, 2.0f, ActorType::ItemEntity).size());
}

ChestBlockActor* chestAt(BlockSource& region, BlockPos const& pos) {
    auto* a = region.getBlockEntity(pos);
    return a && a->getType() == BlockActorType::Chest ? static_cast<ChestBlockActor*>(a) : nullptr;
}

/// Brings the player to `feet` (survival, empty hands) and waits until the area is loaded.
ll::coro::CoroTask<bool> setup(Context& ctx, lo::core::GlobalPos feet) {
    auto& p = ctx.player();
    if (!co_await ctx.ensureLoaded(feet)) co_return false;
    BlockPos f = lo::game::blockPos(feet);
    p.teleport(Vec3{static_cast<float>(f.x) + 0.5f, static_cast<float>(f.y), static_cast<float>(f.z) + 0.5f}, p.getDimensionId());
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(2);
    co_return true;
}

Refusal eval(Context& ctx, lo::core::GlobalPos pos) {
    CarryData d;
    return bp::evaluate(ctx.player(), regionOf(ctx), lo::game::blockPos(pos), d);
}

void expect(Context& ctx, lo::core::GlobalPos pos, Refusal want, std::string const& what) {
    auto got = eval(ctx, pos);
    ctx.check(got == want, what + ": expected " + std::string(bp::refusalName(want)) + ", got " + std::string(bp::refusalName(got)));
}

/// A use-block on the top face through the player's GameMode, as the network handler does for a
/// client's use-item-on transaction (SimulatedPlayer::simulateInteract calls Block::use directly and
/// skips it).
void useOn(Player& p, BlockPos const& pos) {
    ItemStack   held  = p.getSelectedItem();
    auto const& block = p.getDimensionBlockSource().getBlock(pos);
    Vec3        hit{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y) + 1.0f, static_cast<float>(pos.z) + 0.5f};
    p.mGameMode->useItemOn(held, pos, 1, hit, HandSlot::Mainhand, &block, true);
}

/// The real path: a use-block, then two ticks for the deferred pickup.
ll::coro::CoroTask<> click(Context& ctx, lo::core::GlobalPos pos) {
    useOn(ctx.player(), lo::game::blockPos(pos));
    co_await ctx.ticks(2);
}

/// Clears the carry and Slowness and waits for the tick guard.
ll::coro::CoroTask<> drop(Context& ctx) {
    carry::clear(ctx.player());
    carry::removeSlowness(ctx.player());
    co_await ctx.ticks(2);
}
} // namespace

// A full chest picked up through use-block with the vanilla carry key (sneaking).
LO_REGISTER_SELFTEST("blockpickup.chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(100);
    auto    at   = offset(feet, 1);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    auto&    region = regionOf(ctx);
    BlockPos pos    = lo::game::blockPos(at);
    ctx.require(ctx.setBlock(at, "minecraft:chest"), "chest placed");
    co_await ctx.ticks(1);
    auto box = region.tryGetContainer(pos);
    ctx.require(box.has_value(), "the chest has a container");
    box->setItem(0, lo::game::makeItem("minecraft:diamond", 7));
    box->setItem(5, lo::game::makeItem("minecraft:stick", 32));

    co_await click(ctx, at);
    ctx.check(!carry::isCarrying(p), "no pickup without sneaking");
    ctx.check(region.getBlock(pos).getTypeName() == "minecraft:chest", "the chest stays");

    ctx.require(simOf(ctx).simulateSneaking(), "sneaking");
    co_await ctx.ticks(1);
    co_await click(ctx, at);
    ctx.require(carry::isCarrying(p), "sneak + use-block picks the chest up");
    auto data = carry::get(p);
    ctx.check(data.isBlock(), "a block carry");
    ctx.check(carry::carriedBlockId(data) == "minecraft:chest", "the carried block is a chest");
    ctx.check(data.blockEntity.find("minecraft:diamond") != std::string::npos, "the diamonds are carried");
    ctx.check(data.blockEntity.find("minecraft:stick") != std::string::npos, "the sticks are carried");
    ctx.check(region.getBlock(pos).isAir(), "the chest is gone");
    ctx.check(itemsNear(region, pos) == 0, "nothing dropped");
    ctx.check(!bp::isPending(p), "nothing pending");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == carry::blockAmplifier(carry::get(p)), "Slowness from the record size");

    // The carried chest restores with its items (the capture is lossless).
    co_await drop(ctx);
    ctx.require(carry::restoreBlock(region, pos, data), "restored");
    co_await ctx.ticks(1);
    auto back = region.tryGetContainer(pos);
    ctx.require(back.has_value(), "restored chest has a container");
    ctx.check(back->getItem(0).mCount == 7 && back->getItem(5).mCount == 32, "contents kept");
    back->removeAllItems();
});

// Steps 1-2: carry key, distance with reach scale, game mode, hands, carrying, tick guard.
LO_REGISTER_SELFTEST("blockpickup.preconditions", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(110);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    auto near = offset(feet, 1);
    auto edge = offset(feet, 2, 1); // distSq 6.25: out of 2.5 reach, inside 2.5 * 5/4.5
    auto far  = offset(feet, 4);
    ctx.require(ctx.setBlock(near, "minecraft:barrel") && ctx.setBlock(edge, "minecraft:barrel") && ctx.setBlock(far, "minecraft:barrel"), "barrels placed");

    expect(ctx, near, Refusal::Preconditions, "no carry key");
    carry::setKeyPressed(p, true);
    expect(ctx, near, Refusal::None, "key, empty hands, in reach");
    expect(ctx, edge, Refusal::Preconditions, "survival reach");
    expect(ctx, far, Refusal::Preconditions, "far away");

    p.setPlayerGameType(GameType::Creative);
    expect(ctx, edge, Refusal::None, "creative reach is scaled");
    expect(ctx, far, Refusal::Preconditions, "far away in creative");
    settings().scaleDistance = false;
    expect(ctx, edge, Refusal::Preconditions, "no scaling without scaleDistance");
    settings().scaleDistance = true;
    settings().maxDistance   = 5.0;
    p.setPlayerGameType(GameType::Survival);
    expect(ctx, far, Refusal::None, "maxDistance is read from the config");
    settings().maxDistance = 2.5;

    p.setPlayerGameType(GameType::Adventure);
    expect(ctx, near, Refusal::Preconditions, "adventure");
    p.setPlayerGameType(GameType::Spectator);
    expect(ctx, near, Refusal::Preconditions, "spectator");
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(1);
    expect(ctx, near, Refusal::None, "survival again");

    ctx.give(p, lo::game::makeItem("minecraft:stick"), p.getSelectedItemSlot());
    expect(ctx, near, Refusal::Preconditions, "an item in the main hand");
    p.getInventory().setItem(p.getSelectedItemSlot(), ItemStack{});
    p.setOffhandSlot(lo::game::makeItem("minecraft:stick"));
    expect(ctx, near, Refusal::Preconditions, "an item in the offhand");
    p.setOffhandSlot(ItemStack{});
    expect(ctx, near, Refusal::None, "empty hands again");

    CarryData d;
    d.type  = carry::CarryType::Block;
    d.block = R"({name:"minecraft:stone"})";
    carry::set(p, d);
    expect(ctx, near, Refusal::Preconditions, "already carrying");
    carry::clear(p);
    expect(ctx, near, Refusal::Preconditions, "tick guard");
    co_await ctx.ticks(1);
    expect(ctx, near, Refusal::None, "next tick");
});

// Steps 5-6: lists (black and white) and double-height blocks; neither is overridable.
LO_REGISTER_SELFTEST("blockpickup.lists", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(120);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto a = offset(feet, 1);
    auto b = offset(feet, 0, 0, 1);

    ctx.require(ctx.setBlock(a, "minecraft:bed"), "bed placed");
    expect(ctx, a, Refusal::Lists, "beds are blacklisted");
    gHooks.override = true;
    expect(ctx, a, Refusal::Lists, "override_checks does not skip the lists");
    gHooks.override = false;
    ctx.require(ctx.setBlock(a, "minecraft:wooden_door"), "door placed");
    expect(ctx, a, Refusal::Lists, "doors are blacklisted (checked before double-height)");

    settings().pickupAllBlocks = true;
    ctx.require(ctx.setBlock(a, "minecraft:small_dripleaf_block"), "small dripleaf placed");
    expect(ctx, a, Refusal::DoubleHeight, "upper_block_bit blocks are double-height");
    gHooks.override = true;
    expect(ctx, a, Refusal::DoubleHeight, "override_checks does not skip double-height");
    gHooks.override            = false;
    settings().pickupAllBlocks = false;

    ctx.require(ctx.setBlock(a, "minecraft:chest") && ctx.setBlock(b, "minecraft:barrel"), "chest and barrel placed");
    expect(ctx, a, Refusal::None, "chests are allowed by default");
    auto& cfg                       = lo::config::mut<carry::CarryConfig>();
    cfg.settings.useWhitelistBlocks = true;
    cfg.whitelist.allowedBlocks     = {"minecraft:barrel"};
    carry::rebuildLists();
    expect(ctx, a, Refusal::Lists, "whitelist mode refuses unlisted blocks");
    expect(ctx, b, Refusal::None, "whitelist mode allows listed blocks");
    cfg.whitelist.allowedBlocks = {"minecraft:bed"};
    carry::rebuildLists();
    ctx.require(ctx.setBlock(a, "minecraft:bed"), "bed placed");
    expect(ctx, a, Refusal::None, "whitelist mode ignores the blacklist");
});

// Step 7: unbreakable blocks (survival only, pickupUnbreakableBlocks, override_checks).
LO_REGISTER_SELFTEST("blockpickup.unbreakable", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(130);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto cmd     = offset(feet, 1);
    auto barrier = offset(feet, 0, 0, 1);
    ctx.require(ctx.setBlock(cmd, "minecraft:command_block") && ctx.setBlock(barrier, "minecraft:barrier"), "blocks placed");

    expect(ctx, cmd, Refusal::Unbreakable, "command block in survival");
    expect(ctx, barrier, Refusal::Unbreakable, "barrier in survival");
    settings().pickupUnbreakableBlocks = true;
    expect(ctx, cmd, Refusal::None, "pickupUnbreakableBlocks");
    expect(ctx, barrier, Refusal::NoBlockEntity, "pickupUnbreakableBlocks: then the block entity rule");
    settings().pickupUnbreakableBlocks = false;
    gHooks.override = true;
    expect(ctx, cmd, Refusal::None, "override_checks skips the unbreakable rule");
    gHooks.override = false;
    p.setPlayerGameType(GameType::Creative);
    expect(ctx, cmd, Refusal::None, "creative picks up unbreakable blocks");
    expect(ctx, barrier, Refusal::NoBlockEntity, "creative: then the block entity rule");
    settings().pickupAllBlocks = true;
    expect(ctx, barrier, Refusal::None, "creative + pickupAllBlocks: a barrier");
    co_return;
});

// Step 8: a block entity is required (Java notion) unless pickupAllBlocks or override_checks.
LO_REGISTER_SELFTEST("blockpickup.block_entity", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(140);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto stone  = offset(feet, 1);
    auto pot    = offset(feet, 0, 0, 1);
    auto barrel = offset(feet, -1);
    ctx.require(ctx.setBlock(stone, "minecraft:stone") && ctx.setBlock(pot, "minecraft:flower_pot") && ctx.setBlock(barrel, "minecraft:barrel"), "blocks placed");
    co_await ctx.ticks(1);
    auto& region = regionOf(ctx);
    ctx.check(region.getBlockEntity(lo::game::blockPos(pot)) != nullptr, "flower pots have a block actor on Bedrock");

    expect(ctx, stone, Refusal::NoBlockEntity, "stone");
    expect(ctx, pot, Refusal::NoBlockEntity, "flower pot (Bedrock-only block actor)");
    expect(ctx, barrel, Refusal::None, "barrel");
    gHooks.override = true;
    expect(ctx, stone, Refusal::None, "override_checks skips the block entity rule");
    gHooks.override            = false;
    settings().pickupAllBlocks = true;
    expect(ctx, stone, Refusal::None, "pickupAllBlocks: stone");

    // Real pickups: stone carries no block actor, the flower pot keeps its Bedrock actor NBT.
    co_await click(ctx, stone);
    ctx.require(carry::isCarrying(p), "stone picked up");
    ctx.check(carry::carriedBlockId(carry::get(p)) == "minecraft:stone" && carry::get(p).blockEntity.empty(), "stone without block actor");
    ctx.check(region.getBlock(lo::game::blockPos(stone)).isAir(), "stone removed");
    co_await drop(ctx);
    co_await click(ctx, pot);
    ctx.require(carry::isCarrying(p), "flower pot picked up");
    ctx.check(!carry::get(p).blockEntity.empty(), "the flower pot actor NBT is captured");
});

// Step 9: Carry On's lock test (presence of the `Lock` key).
LO_REGISTER_SELFTEST("blockpickup.locked", [](Context& ctx) {
    ctx.check(bp::hasLock(R"({id:"Chest",Items:[],Lock:""})"), "an empty lock still locks");
    ctx.check(bp::hasLock(R"({id:"Chest",Lock:"key"})"), "a named lock");
    ctx.check(!bp::hasLock(R"({id:"Chest",Items:[]})"), "no Lock key");
    ctx.check(!bp::hasLock(""), "no block actor");
});

// Steps 10-12 through the spine hooks; the built-in protection event; their order.
LO_REGISTER_SELFTEST("blockpickup.hooks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(150);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto barrel = offset(feet, 1);
    auto bed    = offset(feet, 0, 0, 1);
    ctx.require(ctx.setBlock(barrel, "minecraft:barrel") && ctx.setBlock(bed, "minecraft:bed"), "blocks placed");

    gHooks.veto = true;
    expect(ctx, barrel, Refusal::CustomCondition, "custom condition not met");
    expect(ctx, bed, Refusal::Lists, "lists come before custom conditions");
    ctx.check(gHooks.protectionCalls == 0, "no protection check after an earlier refusal");
    gHooks.veto    = false;
    gHooks.protect = true;
    expect(ctx, barrel, Refusal::Protection, "a protection hook vetoes");
    gHooks.protect = false;

    // The built-in protection publishes a cancellable PlayerDestroyBlockEvent.
    int  seen = 0;
    bool flag = false;
    auto& bus = ll::event::EventBus::getInstance();
    auto  l   = bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>([&](ll::event::PlayerDestroyBlockEvent& ev) {
        if (&ev.self() != &p) return;
        ++seen;
        flag = carry::inProtectionCheck();
        ev.cancel();
    });
    expect(ctx, barrel, Refusal::Protection, "a cancelled destroy event refuses");
    bus.removeListener(l);
    ctx.check(seen == 1 && flag, "the destroy event is published inside inProtectionCheck()");
    expect(ctx, barrel, Refusal::None, "allowed without the listener");

    gHooks.script = true;
    expect(ctx, barrel, Refusal::Script, "script conditions not met");
    gHooks.script       = false;
    gHooks.activeScript = "selftest_script";
    CarryData d;
    ctx.check(bp::evaluate(p, regionOf(ctx), lo::game::blockPos(barrel), d) == Refusal::None, "allowed");
    ctx.check(d.activeScript == "selftest_script", "beforePickup may activate a script");
    ctx.check(d.isBlock() && carry::carriedBlockId(d) == "minecraft:barrel", "the data holds the block");

    // A refused attempt changes nothing; an accepted one through the real path keeps the script.
    gHooks.protect = true;
    co_await click(ctx, barrel);
    ctx.check(!carry::isCarrying(p), "refused silently");
    ctx.check(regionOf(ctx).getBlock(lo::game::blockPos(barrel)).getTypeName() == "minecraft:barrel", "the barrel stays");
    gHooks.protect = false;
    co_await click(ctx, barrel);
    ctx.require(carry::isCarrying(p), "picked up");
    ctx.check(carry::get(p).activeScript == "selftest_script", "the script is active in the carry");
});

// A large chest: one half is carried with its own 27 slots; the other half stays a single chest.
LO_REGISTER_SELFTEST("blockpickup.double_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(160);
    auto    a    = offset(feet, 1);
    auto    b    = offset(feet, 2);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    ctx.require(ctx.setBlock(a, "minecraft:chest") && ctx.setBlock(b, "minecraft:chest"), "two chests placed");
    co_await ctx.ticks(3);
    auto&    region = regionOf(ctx);
    BlockPos pa = lo::game::blockPos(a), pb = lo::game::blockPos(b);
    auto*    ca = chestAt(region, pa);
    auto*    cb = chestAt(region, pb);
    ctx.require(ca && cb, "both are chests");
    ctx.require(ca->mLargeChestPaired != nullptr, "the chests are paired");
    auto lead = ca->mPairLead ? a : b;
    auto other = ca->mPairLead ? b : a;
    auto large = region.tryGetContainer(lo::game::blockPos(lead));
    ctx.require(large.has_value() && large->getContainerSize() == 54, "a 54-slot large chest");
    large->setItem(0, lo::game::makeItem("minecraft:gold_ingot", 3));  // lead half
    large->setItem(27, lo::game::makeItem("minecraft:iron_ingot", 4)); // other half

    co_await click(ctx, lead);
    ctx.require(carry::isCarrying(p), "the lead half is picked up");
    auto const& be = carry::get(p).blockEntity;
    ctx.check(be.find("minecraft:gold_ingot") != std::string::npos, "the carried half holds its own items");
    ctx.check(be.find("minecraft:iron_ingot") == std::string::npos, "the other half's items are not carried");
    BlockPos po = lo::game::blockPos(other);
    ctx.check(region.getBlock(lo::game::blockPos(lead)).isAir(), "the carried half is gone");
    auto* rest = chestAt(region, po);
    ctx.require(rest != nullptr, "the other half stays");
    ctx.check(rest->mLargeChestPaired == nullptr, "the other half is a single chest");
    auto* k = static_cast<Container*>(rest);
    ctx.check(k->getContainerSize() == 27, "27 slots left");
    ctx.check(k->getItem(0).getTypeName() == "minecraft:iron_ingot" && k->getItem(0).mCount == 4, "the other half keeps its items");
    ctx.check(itemsNear(region, pa) == 0 && itemsNear(region, pb) == 0, "nothing dropped");
    k->removeAllItems();
});

// Step 16: Slowness in creative follows slownessInCreative.
LO_REGISTER_SELFTEST("blockpickup.creative", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(170);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    p.setPlayerGameType(GameType::Creative);
    auto a = offset(feet, 1);
    auto b = offset(feet, 0, 0, 1);
    ctx.require(ctx.setBlock(a, "minecraft:barrel") && ctx.setBlock(b, "minecraft:barrel"), "barrels placed");
    co_await ctx.ticks(1);

    co_await click(ctx, a);
    ctx.require(carry::isCarrying(p), "creative picks up");
    ctx.check(carry::slownessAmplifier(p) == carry::blockAmplifier(carry::get(p)), "Slowness in creative by default");
    co_await drop(ctx);

    settings().slownessInCreative = false;
    co_await click(ctx, b);
    ctx.require(carry::isCarrying(p), "creative picks up again");
    ctx.check(carry::slownessAmplifier(p) == -1, "no Slowness in creative when slownessInCreative is false");
});

// Step 16: block amplifier = min(4, record size / 500) x multiplier; heavyTiles false = 1.
LO_REGISTER_SELFTEST("blockpickup.slowness", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(180);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto full  = offset(feet, 1);
    auto light = offset(feet, 0, 0, 1);
    ctx.require(ctx.setBlock(full, "minecraft:chest") && ctx.setBlock(light, "minecraft:barrel"), "blocks placed");
    co_await ctx.ticks(1);
    auto& region = regionOf(ctx);
    auto  box    = region.tryGetContainer(lo::game::blockPos(full));
    ctx.require(box.has_value(), "chest container");
    for (int i = 0; i < 27; ++i) box->setItem(i, lo::game::makeItem("minecraft:diamond", 64));

    co_await click(ctx, full);
    ctx.require(carry::isCarrying(p), "the full chest is picked up");
    auto size = carry::get(p).nbtSize();
    int  want = static_cast<int>(std::min<std::size_t>(4, size / 500));
    ctx.info("record size " + std::to_string(size));
    ctx.check(want >= 3, "a full chest is heavy");
    ctx.check(carry::slownessAmplifier(p) == want, "amplifier = min(4, size / 500)");
    settings().blockSlownessMultiplier = 0.5;
    ctx.check(carry::blockAmplifier(carry::get(p)) == static_cast<int>(want * 0.5), "blockSlownessMultiplier");
    settings().blockSlownessMultiplier = 1.0;
    co_await drop(ctx);

    settings().heavyTiles = false;
    co_await click(ctx, light);
    ctx.require(carry::isCarrying(p), "the barrel is picked up");
    ctx.check(carry::slownessAmplifier(p) == 1, "heavyTiles false: Slowness II");
});

// Step 13-14: water stays in the world, the carried state has none.
LO_REGISTER_SELFTEST("blockpickup.waterlogged", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(190);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto at = offset(feet, 1);
    ctx.require(ctx.setBlock(at, "minecraft:chest"), "chest placed");
    auto&    region = regionOf(ctx);
    BlockPos pos    = lo::game::blockPos(at);
    auto const* water = lo::game::blockWith("minecraft:water");
    ctx.require(water != nullptr, "water exists");
    region.setExtraBlock(pos, *water, 3);
    co_await ctx.ticks(1);
    ctx.require(region.getExtraBlock(pos).getTypeName() == "minecraft:water", "the chest holds water");

    co_await click(ctx, at);
    ctx.require(carry::isCarrying(p), "the waterlogged chest is picked up");
    ctx.check(carry::get(p).block.find("water") == std::string::npos, "no water in the carried state");
    ctx.check(region.getBlock(pos).getTypeName() == "minecraft:water", "the water stays");
    lo::game::clearBlock(region, pos);
});

// Repeated use-block in one tick: one pickup; the click is cancelled while pending.
LO_REGISTER_SELFTEST("blockpickup.repeat", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&   p    = ctx.player();
    Restore restore{p};
    auto    feet = ctx.testPos(195);
    ctx.require(co_await setup(ctx, feet), "test area loaded");
    carry::setKeyPressed(p, true);
    auto at = offset(feet, 1);
    ctx.require(ctx.setBlock(at, "minecraft:barrel"), "barrel placed");
    co_await ctx.ticks(1);
    BlockPos pos = lo::game::blockPos(at);
    useOn(p, pos);
    ctx.check(bp::isPending(p), "accepted pickup waits for the next tick");
    ctx.check(!carry::isCarrying(p), "nothing carried inside the use-block event");
    useOn(p, pos);
    co_await ctx.ticks(2);
    ctx.require(carry::isCarrying(p), "picked up");
    ctx.check(!bp::isPending(p), "nothing pending");
    ctx.check(regionOf(ctx).getBlock(pos).isAir(), "the barrel is gone");
});
