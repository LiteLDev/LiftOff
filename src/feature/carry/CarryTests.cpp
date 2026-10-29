// Selftests of the carry spine (U0, dx 0..99): config, lists, data, store, slowness, capture /
// restore of blocks and entities, forced placement, protocol.

#include <algorithm>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/ChestBlockActor.h"
#include "mc/world/phys/AABB.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/PlaceCarried.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
using carry::CarryData;
using carry::CarryType;

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

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

/// Puts the test player at `pos` (feet), facing south.
void standAt(Player& p, BlockPos const& pos) {
    p.teleport(Vec3{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f}, p.getDimensionId());
}
} // namespace

LO_REGISTER_SELFTEST("carry.config", [](Context& ctx) {
    auto const& cfg = lo::config::get<carry::CarryConfig>();
    ctx.check(cfg.version >= 1, "carryon.json is loaded");
    ctx.check(cfg.settings.maxDistance == 2.5, "maxDistance defaults to 2.5");
    ctx.check(cfg.settings.maxEntityStackLimit == 10, "maxEntityStackLimit defaults to 10");
    ctx.check(!cfg.settings.pickupAllBlocks && cfg.settings.pickupPlayers, "boolean defaults");
    ctx.check(cfg.blacklist.forbiddenTiles.size() == 145, "145 default forbidden tiles");
    ctx.check(cfg.blacklist.forbiddenEntities.size() == 41, "41 default forbidden entities");
    ctx.check(cfg.blacklist.forbiddenStacking.size() == 2, "2 default forbidden stacking entries");
    auto const& client = lo::config::get<carry::CarryClientConfig>();
    ctx.check(client.settings.renderArms && !client.settings.facePlayer, "client config defaults");
    ctx.check(client.modelOverrides.modelOverrides.size() == 3, "3 default model overrides");
    ctx.check(lo::tr("carryon.category.settings", "?") == "Settings", "Carry On strings are loaded");
});

LO_REGISTER_SELFTEST("carry.lists", [](Context& ctx) {
    ctx.check(!carry::isBlockIdAllowed("minecraft:bed"), "beds are blacklisted");
    ctx.check(!carry::isBlockIdAllowed("minecraft:spruce_door"), "doors are blacklisted (wildcard)");
    ctx.check(!carry::isBlockIdAllowed("create:anything"), "modded wildcard entries are kept");
    ctx.check(carry::isBlockIdAllowed("minecraft:chest"), "chests are allowed");
    ctx.check(!carry::isEntityIdAllowed("minecraft:ender_dragon"), "the ender dragon is blacklisted");
    ctx.check(carry::isEntityIdAllowed("minecraft:pig"), "pigs are allowed");
    ctx.check(carry::isPlacementException("vine_direction_bits"), "placement exceptions are compiled");
    ctx.check(!carry::isPlacementException("cardinal_direction"), "unknown states are no exception");

    auto const* stem = Block::tryGetFromRegistry(HashedString("minecraft:big_dripleaf")).as_ptr();
    ctx.require(stem != nullptr, "big_dripleaf exists");
    auto head = carry::blockStateValue(*stem, "big_dripleaf_head");
    ctx.check(head.has_value(), "big_dripleaf has big_dripleaf_head");
    // The default dripleaf state is a stem or a head; the list refuses exactly the stem.
    ctx.check(carry::isBlockAllowed(*stem) == (head == "true"), "state filters of list entries apply");

    auto const* door = lo::game::blockWith("minecraft:wooden_door");
    ctx.require(door != nullptr, "wooden_door exists");
    ctx.check(carry::isDoubleHeight(*door), "doors are double-height");
    auto const* chest = lo::game::blockWith("minecraft:chest");
    ctx.require(chest != nullptr, "chest exists");
    ctx.check(!carry::isDoubleHeight(*chest), "chests are not double-height");
    ctx.check(carry::isBlockAllowed(*chest), "a chest block is allowed");
    auto const* bedrock = lo::game::blockWith("minecraft:bedrock");
    ctx.check(bedrock && carry::isUnbreakable(*bedrock), "bedrock is unbreakable");
    ctx.check(!carry::isUnbreakable(*chest), "chests are breakable");
    ctx.check(carry::isBedrockOnlyBlockActor("minecraft:flower_pot"), "flower pots have a Bedrock-only block actor");
});

LO_REGISTER_SELFTEST("carry.data", [](Context& ctx) {
    CarryData d;
    d.type         = CarryType::Block;
    d.block        = R"({name:"minecraft:chest",states:{"minecraft:cardinal_direction":"south"},version:1})";
    d.blockEntity  = R"({id:"Chest",Items:[],x:1,y:2,z:3})";
    d.activeScript = "test";
    d.scriptRender = {{"name_block", "minecraft:stone"}};
    d.selected     = 4;
    d.tick         = 1234;
    auto back      = CarryData::fromJson(d.toJson());
    ctx.check(back == d, "JSON round trip keeps every field");
    ctx.check(CarryData::fromJson(nlohmann::json::array()).type == CarryType::Invalid, "garbage gives an empty carry");
    ctx.check(d.nbtSize() > d.block.size() + d.blockEntity.size(), "nbtSize covers the whole record");
    auto copy = d;
    copy.clear();
    ctx.check(!copy.isCarrying() && copy.selected == 4 && copy.tick == 1234, "clear keeps selected and tick");
});

LO_REGISTER_SELFTEST("carry.store", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p), "a new player carries nothing");
    int  changes = 0;
    bool active  = true;
    carry::onCarryChanged([&changes, &active, uuid = p.getUuid().asString()](Player& who, CarryData const&, CarryData const&) {
        if (active && who.getUuid().asString() == uuid) ++changes;
    });
    p.setSelectedSlot(3);
    CarryData d;
    d.type  = CarryType::Entity;
    d.entity = R"({identifier:"minecraft:pig"})";
    carry::set(p, d);
    ctx.check(carry::isCarrying(p) && carry::get(p).isEntity(), "set stores the carry");
    ctx.check(carry::get(p).selected == 3, "set records the selected slot");
    ctx.check(carry::get(p).tick == carry::tickCount(p), "set records the tick");
    ctx.check(!carry::canAct(p), "tick guard blocks the same tick");
    auto stored = lo::game::db::load("lo:carry:" + p.getUuid().asString());
    ctx.check(stored && stored->value("type", "") == "entity", "the carry is persisted in the world DB");
    co_await ctx.ticks(2);
    ctx.check(carry::canAct(p), "tick guard passes a later tick");
    carry::clear(p);
    ctx.check(!carry::isCarrying(p), "clear empties the carry");
    ctx.check(!lo::game::db::load("lo:carry:" + p.getUuid().asString()), "clear erases the DB record");
    ctx.check(changes == 2, "listeners see set and clear");
    active = false;

    ctx.check(!carry::keyPressed(p), "key not pressed while not sneaking");
    carry::setKeyPressed(p, true);
    ctx.check(carry::keyPressed(p) && carry::get(p).keyPressed, "setKeyPressed overrides");
    carry::resetKeyPressed(p);
    ctx.check(!carry::keyPressed(p), "reset goes back to sneaking");
});

LO_REGISTER_SELFTEST("carry.slowness", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    CarryData small;
    small.type  = CarryType::Block;
    small.block = R"({name:"minecraft:stone"})";
    ctx.check(carry::blockAmplifier(small) == 0, "a small block gives Slowness I");
    CarryData big       = small;
    big.blockEntity     = std::string(2400, 'x');
    ctx.check(carry::blockAmplifier(big) == 4, "a 2400+ char record gives Slowness V");
    ctx.check(carry::playerAmplifier() == 1, "players give Slowness II");

    carry::applySlowness(p, 2);
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == 2, "Slowness III is applied");
    carry::removeSlowness(p);
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness is removed");
});

LO_REGISTER_SELFTEST("carry.pickup_checks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(5);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    co_await ctx.ticks(2);
    Vec3 near = BlockPos{feet.x + 1, feet.y, feet.z}.center();
    Vec3 far  = BlockPos{feet.x + 4, feet.y, feet.z}.center();
    ctx.check(!carry::canStartPickup(p, near, false), "no pickup without the carry key");
    carry::setKeyPressed(p, true);
    ctx.check(carry::canStartPickup(p, near, false), "pickup allowed: key, empty hands, in reach");
    ctx.check(!carry::canStartPickup(p, far, false), "out of reach");
    ctx.give(p, lo::game::makeItem("minecraft:stick"), p.getSelectedItemSlot());
    ctx.check(!carry::canStartPickup(p, near, false), "a held item prevents pickup");
    p.getInventory().setItem(p.getSelectedItemSlot(), ItemStack{});
    CarryData d;
    d.type  = CarryType::Block;
    d.block = R"({name:"minecraft:stone"})";
    carry::set(p, d);
    ctx.check(!carry::canStartPickup(p, near, false), "already carrying");
    carry::clear(p);
    ctx.check(!carry::canStartPickup(p, near, false), "tick guard after a change");
    co_await ctx.ticks(1);
    ctx.check(carry::canStartPickup(p, near, false), "allowed again next tick");
    carry::resetKeyPressed(p);
});

LO_REGISTER_SELFTEST("carry.capture_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto from = ctx.testPos(11);
    auto to   = ctx.testPos(15);
    ctx.require(co_await ctx.ensureLoaded(from), "test area loaded");
    ctx.require(ctx.setBlock(from, "minecraft:chest"), "chest placed");
    ctx.cleanupBlock(to);
    co_await ctx.ticks(1);
    auto&    region = regionOf(ctx);
    BlockPos src    = lo::game::blockPos(from);
    BlockPos dst    = lo::game::blockPos(to);
    auto     box    = region.tryGetContainer(src);
    ctx.require(box.has_value(), "the chest has a container");
    box->setItem(0, lo::game::makeItem("minecraft:diamond", 7));
    box->setItem(5, lo::game::makeItem("minecraft:stick", 32));

    auto cap = carry::captureBlock(region, src);
    ctx.require(cap.has_value() && cap->hasBlockEntity(), "block and block actor captured");
    auto nbt = carry::parseNbt(cap->blockEntity);
    ctx.require(nbt.has_value(), "block actor SNBT parses");
    ctx.check(carry::toSnbt(*nbt) == cap->blockEntity, "block actor SNBT round-trips");
    ctx.check(carry::removeCarriedBlock(region, src), "block removed");
    co_await ctx.ticks(2);
    ctx.check(region.getBlock(src).isAir(), "the chest is gone");
    ctx.check(itemsNear(region, src) == 0, "nothing dropped");

    CarryData d;
    d.type        = CarryType::Block;
    d.block       = cap->block;
    d.blockEntity = cap->blockEntity;
    ctx.check(carry::carriedBlockId(d) == "minecraft:chest", "carried block id");
    ctx.check(carry::carriedBlock(d) != nullptr, "carried block state resolves");
    ctx.require(carry::restoreBlock(region, dst, d), "block restored");
    co_await ctx.ticks(1);
    ctx.check(region.getBlock(dst).getTypeName() == "minecraft:chest", "the chest is at the new place");
    auto restored = region.tryGetContainer(dst);
    ctx.require(restored.has_value(), "the restored chest has a container");
    ctx.check(restored->getItem(0).mCount == 7 && restored->getItem(0).getTypeName() == "minecraft:diamond", "slot 0 kept");
    ctx.check(restored->getItem(5).mCount == 32 && restored->getItem(5).getTypeName() == "minecraft:stick", "slot 5 kept");
    restored->removeAllItems(); // no drops at cleanup
});

LO_REGISTER_SELFTEST("carry.double_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a = ctx.testPos(20);
    auto b = ctx.testPos(21);
    ctx.require(co_await ctx.ensureLoaded(a), "test area loaded");
    ctx.require(ctx.setBlock(a, "minecraft:chest") && ctx.setBlock(b, "minecraft:chest"), "two chests placed");
    co_await ctx.ticks(3);
    auto&    region = regionOf(ctx);
    BlockPos pa = lo::game::blockPos(a), pb = lo::game::blockPos(b); // pa = the carried half
    auto*    ca = chestAt(region, pa);
    ctx.require(ca != nullptr && chestAt(region, pb) != nullptr, "both are chests");
    bool pairedAtStart = ca->mLargeChestPaired != nullptr;
    ctx.info(std::string("adjacent chests paired after setBlock: ") + (pairedAtStart ? "yes" : "no"));
    // A large chest is one 54-slot container on both halves: slots 0..26 live in the lead half, 27..53
    // in the other one.
    auto* lead  = ca->mPairLead ? ca : chestAt(region, pb);
    auto  large = region.tryGetContainer(lo::game::blockPos(lead == ca ? a : b));
    ctx.require(large.has_value(), "large chest container");
    large->setItem(0, lo::game::makeItem("minecraft:gold_ingot", 3));
    large->setItem(27, lo::game::makeItem("minecraft:iron_ingot", 4));
    if (lead != ca) std::swap(pa, pb); // carry the lead half (holds the gold)

    auto describe = [&](char const* when) {
        auto* x = chestAt(region, pa);
        auto* y = chestAt(region, pb);
        auto  one = [](ChestBlockActor* c) {
            if (!c) return std::string("none");
            auto* k = static_cast<Container*>(c);
            return "size " + std::to_string(k->getContainerSize()) + " s0 " + std::to_string(k->getItem(0).mCount) + " "
                 + k->getItem(0).getTypeName() + " paired " + (c->mLargeChestPaired ? "yes" : "no") + " lead "
                 + (c->mPairLead ? "yes" : "no");
        };
        ctx.info(std::string(when) + ": a[" + one(x) + "] b[" + one(y) + "]");
    };
    describe("before");
    auto cap = carry::captureBlock(region, pa);
    ctx.require(cap.has_value(), "captured");
    carry::removeCarriedBlock(region, pa);
    co_await ctx.ticks(2);
    describe("after removal");
    auto* cb = chestAt(region, pb);
    ctx.require(cb != nullptr, "the other half stays");
    ctx.check(cb->mLargeChestPaired == nullptr, "the other half is a single chest now");
    ctx.check(static_cast<Container*>(cb)->getItem(0).mCount == 4, "the other half keeps its own items");
    ctx.check(static_cast<Container*>(cb)->getContainerSize() == 27, "the other half is a single chest");
    ctx.check(itemsNear(region, pa) == 0, "nothing dropped");

    CarryData d;
    d.type        = CarryType::Block;
    d.block       = cap->block;
    d.blockEntity = cap->blockEntity;
    ctx.require(carry::restoreBlock(region, pa, d), "restored next to the other half");
    co_await ctx.ticks(2);
    describe("after restore");
    ca = chestAt(region, pa);
    cb = chestAt(region, pb);
    ctx.require(ca && cb, "both are chests again");
    if (pairedAtStart) ctx.check(ca->mLargeChestPaired != nullptr, "the chests are paired again");
    ctx.check(static_cast<Container*>(ca)->getItem(0).getTypeName() == "minecraft:gold_ingot", "the carried half keeps its items");
    static_cast<Container*>(ca)->removeAllItems();
    static_cast<Container*>(cb)->removeAllItems();
});

LO_REGISTER_SELFTEST("carry.capture_actor", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(30);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = regionOf(ctx);
    Vec3  at     = lo::game::blockPos(pos).bottomCenter();
    auto* pig    = level->getSpawner().spawnMob(region, ActorDefinitionIdentifier("minecraft:pig"), nullptr, at, false, false, false);
    ctx.require(pig != nullptr, "a pig spawned");
    pig->setNameTag("lo_carry_pig");
    co_await ctx.ticks(2);
    auto id     = pig->getOrCreateUniqueID();
    int  health = pig->getHealth();

    auto snbt = carry::captureActor(*pig);
    ctx.check(carry::entityIdOf(snbt) == "minecraft:pig", "entity id from the NBT");
    ctx.check(carry::parseNbt(snbt).has_value(), "entity SNBT parses");
    carry::removeCapturedActor(*pig);
    co_await ctx.ticks(2);
    ctx.check(level->fetchEntity(id, false) == nullptr, "the pig is removed");

    Vec3  there = lo::game::blockPos(ctx.testPos(34)).bottomCenter();
    auto* back  = carry::spawnActor(region, there, snbt);
    ctx.require(back != nullptr, "the pig is recreated");
    co_await ctx.ticks(1);
    ctx.check(back->getOrCreateUniqueID() == id, "the unique id is kept");
    ctx.check(back->getNameTag() == "lo_carry_pig", "the name tag is kept");
    ctx.check(back->getHealth() == health, "the health is kept");
    ctx.check(back->getPosition().distanceTo(there) < 1.0f, "spawned at the requested position");
    back->remove();
});

LO_REGISTER_SELFTEST("carry.place_carried", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(40);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto&    region = regionOf(ctx);
    BlockPos feet   = lo::game::blockPos(pos);
    standAt(p, feet);
    co_await ctx.ticks(2);
    CarryData d;
    d.type  = CarryType::Block;
    d.block = carry::toSnbt(*lo::game::blockWith("minecraft:barrel")->mSerializationId);
    carry::set(p, d);
    carry::applySlowness(p, 1);
    auto r = carry::placeCarried(p);
    ctx.require(r.placed, "the carry was placed");
    BlockPos at{r.x, r.y, r.z};
    ctx.cleanupBlock({0, at.x, at.y, at.z});
    ctx.check(region.getBlock(at).getTypeName() == "minecraft:barrel", "a barrel is at the reported position");
    int dist2 = (at.x - feet.x) * (at.x - feet.x) + (at.y - feet.y) * (at.y - feet.y) + (at.z - feet.z) * (at.z - feet.z);
    ctx.check(dist2 <= 2, "placed right next to the player");
    ctx.check(!(at == feet), "not inside the player");
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness is removed");
    ctx.check(!carry::placeCarried(p).placed, "nothing to place afterwards");
});

LO_REGISTER_SELFTEST("carry.net", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace net = lo::net;
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    ctx.check(!net::isModded(p), "simulated players are vanilla clients");
    net::setTestModded(p, true);
    ctx.check(net::isModded(p), "test seam marks the player modded");
    (void)net::takeTestMessages(p);

    CarryData d;
    d.type  = CarryType::Block;
    d.block = R"({name:"minecraft:stone"})";
    carry::set(p, d);
    auto msgs = net::takeTestMessages(p);
    auto sync = std::find_if(msgs.begin(), msgs.end(), [](auto const& m) { return m.first.find("CarrySyncPacket") != std::string::npos; });
    ctx.check(sync != msgs.end(), "a change is synced to modded players");
    if (sync != msgs.end()) {
        ctx.check(sync->second.value("carrier", std::int64_t{0}) == p.getOrCreateUniqueID().rawID, "sync names the carrier");
        ctx.check(CarryData::fromJson(sync->second["data"]).isBlock(), "sync carries the data");
    }
    net::injectFromClient(p, "KeyPressedPacket", {{"pressed", true}});
    ctx.check(carry::keyPressed(p), "KeyPressedPacket sets the key");
    net::injectFromClient(p, "KeyPressedPacket", {{"pressed", false}});
    ctx.check(!carry::keyPressed(p), "KeyPressedPacket releases the key");

    net::injectFromClient(p, "HelloPacket", {{"protocol", net::kProtocolVersion}});
    msgs = net::takeTestMessages(p);
    ctx.check(std::any_of(msgs.begin(), msgs.end(), [](auto const& m) { return m.first.find("WelcomePacket") != std::string::npos; }), "hello is welcomed");
    carry::clear(p);
    carry::resetKeyPressed(p);
    net::setTestModded(p, false);
    ctx.check(!net::isModded(p), "unmarked again");
});

LO_REGISTER_SELFTEST("carry.hooks", [](Context& ctx) {
    auto&        p = ctx.player();
    CarryData    d;
    carry::PickupTarget t{p, regionOf(ctx)};
    ctx.check(!carry::overrideChecks(t), "no hook overrides checks by default");
    ctx.check(carry::customConditions(t), "custom conditions met by default");
    ctx.check(carry::beforePickup(t, d), "beforePickup passes by default");
    ctx.check(!carry::inProtectionCheck(), "not inside a protection check");
    d.type  = CarryType::Block;
    d.block = R"({name:"minecraft:stone"})";
    auto v  = carry::visualFor(d);
    ctx.check(v.kind == carry::CarryVisual::Kind::Block && v.block == d.block, "default visual is the carried block");
});
