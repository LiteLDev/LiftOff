// Selftests of U6 restrictions (dx 600..699): hotbar lock, items never entering the locked slot,
// pick block, use item, attack / destroy vs hitWhileCarrying, containers, dropCarriedWhenHit, and a
// real-player check for the client e2e scenario.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerUseItemEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/BlockPickRequestPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/managers/models/ContainerManagerModel.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/restrictions/Restrictions.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry        = lo::feature::carry;
namespace restrictions = lo::feature::restrictions;
using carry::CarryData;
using carry::CarryType;
using ScriptModuleMinecraft::ScriptFacing;

/// Gives `p` a carried stone block (locks the current hotbar slot).
void startCarry(Player& p, char const* block = R"({name:"minecraft:stone"})") {
    CarryData d;
    d.type  = CarryType::Block;
    d.block = block;
    carry::set(p, d);
}

/// Clears the carry of a player when the test ends (also when it aborts).
class CarryGuard {
public:
    explicit CarryGuard(Player& p) : mId(p.getOrCreateUniqueID().rawID) {}
    ~CarryGuard() {
        auto level = ll::service::getLevel();
        if (!level) return;
        if (auto* p = level->getPlayer(ActorUniqueID(mId))) carry::clear(*p);
    }
    CarryGuard(CarryGuard const&)            = delete;
    CarryGuard& operator=(CarryGuard const&) = delete;

private:
    std::int64_t mId;
};

/// Sets a carryon.json setting for the test and restores it afterwards.
class SettingGuard {
public:
    SettingGuard(bool carry::CarryConfig::Settings::* field, bool value) : mField(field) {
        auto& s = lo::config::mut<carry::CarryConfig>().settings;
        mOld    = s.*mField;
        s.*mField = value;
    }
    ~SettingGuard() { lo::config::mut<carry::CarryConfig>().settings.*mField = mOld; }
    void set(bool value) { lo::config::mut<carry::CarryConfig>().settings.*mField = value; }
    SettingGuard(SettingGuard const&)            = delete;
    SettingGuard& operator=(SettingGuard const&) = delete;

private:
    bool carry::CarryConfig::Settings::* mField;
    bool                                 mOld = false;
};

void standAt(Player& p, BlockPos const& pos) {
    p.teleport(Vec3{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f}, p.getDimensionId());
}

ServerNetworkHandler* networkHandler() {
    auto h = ll::service::getServerNetworkHandler();
    return h ? h.as_ptr() : nullptr;
}

/// Hands a MobEquipmentPacket (hotbar slot change) to the server as if `p`'s client had sent it.
bool sendSlotChange(Player& p, int slot) {
    auto* handler = networkHandler();
    auto  base    = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerEquipment);
    if (!handler || !base) return false;
    auto pkt           = std::static_pointer_cast<MobEquipmentPacket>(base);
    pkt->mRuntimeId    = p.getRuntimeID();
    pkt->mSlot         = slot;
    pkt->mSelectedSlot = slot;
    pkt->mContainerId  = ContainerID::Inventory;
    handler->handle(p.getNetworkIdentifier(), pkt);
    return true;
}

bool sendBlockPick(Player& p, BlockPos const& pos) {
    auto* handler = networkHandler();
    auto  base    = MinecraftPackets::createPacket(MinecraftPacketIds::BlockPickRequest);
    if (!handler || !base) return false;
    auto& pkt     = static_cast<BlockPickRequestPacket&>(*base);
    pkt.mPos      = pos;
    pkt.mWithData = false;
    pkt.mMaxSlots = 9;
    handler->handle(p.getNetworkIdentifier(), pkt);
    return true;
}

int itemsNear(BlockSource& region, Vec3 const& at) {
    AABB box{at - Vec3{2.0f, 2.0f, 2.0f}, at + Vec3{2.0f, 2.0f, 2.0f}};
    return static_cast<int>(region.getEntities(box, 0.0f, ActorType::ItemEntity).size());
}

int snowballsNear(BlockSource& region, Vec3 const& at) {
    AABB box{at - Vec3{16.0f, 16.0f, 16.0f}, at + Vec3{16.0f, 16.0f, 16.0f}};
    return static_cast<int>(region.getEntities(box, 0.0f, ActorType::Snowball).size());
}

Actor* fetch(std::int64_t id) {
    auto level = ll::service::getLevel();
    return level ? level->fetchEntity(ActorUniqueID(id), false) : nullptr;
}

bool hasContainerOpen(Player const& p) { return static_cast<bool>(p.mContainerManager.get()); }
} // namespace

LO_REGISTER_SELFTEST("restrictions.hotbar_lock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    CarryGuard guard(p);
    restrictions::selectSlot(p, 2);
    startCarry(p);
    ctx.require(carry::get(p).selected == 2, "the carry locks slot 2");
    co_await ctx.ticks(1);

    p.setSelectedSlot(5);
    co_await ctx.ticks(2);
    ctx.check(p.getSelectedItemSlot() == 2, "a slot change is snapped back within a tick");

    // The network path: a client's slot change (MobEquipmentPacket) is refused at once.
    carry::clear(p);
    bool sent = sendSlotChange(p, 6);
    ctx.require(sent, "MobEquipmentPacket handed to the server");
    if (p.getSelectedItemSlot() != 6) {
        ctx.info("the server ignored a MobEquipmentPacket of a SimulatedPlayer; packet path not checked");
    } else {
        startCarry(p); // locks 6
        co_await ctx.ticks(1);
        sendSlotChange(p, 3);
        ctx.check(p.getSelectedItemSlot() == 6, "a client slot change to another slot is refused");
        sendSlotChange(p, 6);
        ctx.check(p.getSelectedItemSlot() == 6, "selecting the locked slot is fine");
    }

    carry::clear(p);
    p.setSelectedSlot(4);
    co_await ctx.ticks(2);
    ctx.check(p.getSelectedItemSlot() == 4, "the slot is free again after the carry is cleared");
});

LO_REGISTER_SELFTEST("restrictions.item_pickup", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(600);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    standAt(p, lo::game::blockPos(pos));
    co_await ctx.ticks(2);
    CarryGuard guard(p);
    restrictions::selectSlot(p, 0);
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, ItemStack::EMPTY_ITEM());
    startCarry(p);

    // Player::add (/give, scripts): never the locked slot.
    ItemStack sticks = lo::game::makeItem("minecraft:stick", 5);
    p.add(sticks);
    ctx.check(inv.getItem(0).isNull(), "added items do not go into the locked slot");
    ctx.check(!inv.getItem(1).isNull() && inv.getItem(1).getTypeName() == "minecraft:stick", "they go into the next slot");
    ctx.check(inv.getItem(0).isNull() && inv.getFirstEmptySlot() != 0, "the locked slot is not offered as empty");

    // Only the locked slot free: nothing fits.
    for (int i = 1; i < inv.getContainerSize(); ++i) ctx.give(p, lo::game::makeItem("minecraft:dirt", 64), i);
    ItemStack diamond = lo::game::makeItem("minecraft:diamond", 1);
    ctx.check(!inv.canAdd(diamond), "canAdd is false when only the locked slot is free");
    ItemStack more = lo::game::makeItem("minecraft:diamond", 1);
    p.add(more);
    ctx.check(inv.getItem(0).isNull(), "add leaves the locked slot empty");

    // A ground item stays on the ground.
    auto& region = p.getDimensionBlockSource();
    Vec3  at     = p.getPosition();
    auto* item   = p.getLevel().getSpawner().spawnItem(region, diamond, nullptr, at, 0);
    ctx.require(item != nullptr, "a diamond item spawned");
    co_await ctx.ticks(50);
    ctx.check(inv.getItem(0).isNull(), "a ground item is not picked up into the locked slot");
    ctx.check(itemsNear(region, p.getPosition()) > 0, "the ground item stays on the ground");

    // Not carrying: the same item is picked up into the free slot (the check above is meaningful).
    carry::clear(p);
    co_await ctx.ticks(50);
    ctx.check(!inv.getItem(0).isNull() && inv.getItem(0).getTypeName() == "minecraft:diamond", "picked up once not carrying");
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, ItemStack::EMPTY_ITEM());
});

LO_REGISTER_SELFTEST("restrictions.pick_block", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(610);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    auto target = lo::game::globalPos(p.getDimensionBlockSource(), BlockPos{feet.x + 2, feet.y, feet.z});
    ctx.require(ctx.setBlock(target, "minecraft:dirt"), "dirt placed");
    co_await ctx.ticks(2);
    CarryGuard guard(p);
    auto&      inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, ItemStack::EMPTY_ITEM());
    ctx.give(p, lo::game::makeItem("minecraft:dirt", 3), 4);
    restrictions::selectSlot(p, 0);
    startCarry(p);
    co_await ctx.ticks(1);

    ctx.require(sendBlockPick(p, lo::game::blockPos(target)), "BlockPickRequestPacket handed to the server");
    ctx.check(p.getSelectedItemSlot() == 0 && inv.getItem(0).isNull(), "pick block does nothing while carrying");

    carry::clear(p);
    sendBlockPick(p, lo::game::blockPos(target));
    if (p.getSelectedItemSlot() == 0 && inv.getItem(0).isNull()) {
        ctx.info("vanilla pick block did nothing for a SimulatedPlayer either; refusal not distinguishable");
    } else {
        ctx.info("vanilla pick block works when not carrying");
    }
    restrictions::selectSlot(p, 0);
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, ItemStack::EMPTY_ITEM());
});

LO_REGISTER_SELFTEST("restrictions.use_item", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    CarryGuard guard(p);
    startCarry(p);
    auto& sim = static_cast<SimulatedPlayer&>(p);

    auto& region = p.getDimensionBlockSource();
    ItemStack snow = lo::game::makeItem("minecraft:snowball", 4);
    sim.simulateUseItem(snow);
    co_await ctx.ticks(2);
    ctx.check(snowballsNear(region, p.getPosition()) == 0, "a snowball is not thrown while carrying");

    ItemStack                      other = lo::game::makeItem("minecraft:snowball", 1);
    ll::event::PlayerUseItemEvent  ev(p, other);
    ll::event::EventBus::getInstance().publish(ev);
    ctx.check(ev.isCancelled(), "PlayerUseItemEvent is cancelled while carrying");

    sim.simulateStopUsingItem();
    carry::clear(p);
    co_await ctx.ticks(10);
    ItemStack snow2 = lo::game::makeItem("minecraft:snowball", 4);
    sim.simulateUseItem(snow2);
    bool thrown = false;
    for (int i = 0; i < 5 && !thrown; ++i) {
        thrown = snowballsNear(region, p.getPosition()) > 0;
        if (!thrown) co_await ctx.ticks(1);
    }
    ctx.check(thrown, "a snowball is thrown when not carrying");
});

LO_REGISTER_SELFTEST("restrictions.attack", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(620);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(2);
    auto& region = p.getDimensionBlockSource();
    Vec3  at     = BlockPos{feet.x + 1, feet.y, feet.z}.bottomCenter();
    Actor* pig   = p.getLevel().getSpawner().spawnMob(region, ActorDefinitionIdentifier("minecraft:pig"), nullptr, at, false, false, false);
    ctx.require(pig != nullptr, "a pig spawned");
    std::int64_t pigId = pig->getOrCreateUniqueID().rawID;
    pig->setNameTag("lo_restrictions_pig");

    CarryGuard   guard(p);
    SettingGuard hit(&carry::CarryConfig::Settings::hitWhileCarrying, false);
    startCarry(p);
    co_await ctx.ticks(2);
    auto& sim = static_cast<SimulatedPlayer&>(p);

    pig = fetch(pigId);
    ctx.require(pig != nullptr, "the pig is still there");
    int health = pig->getHealth();
    sim.simulateLookAt(*pig);
    sim.simulateAttack(pig);
    co_await ctx.ticks(2);
    pig = fetch(pigId);
    ctx.require(pig != nullptr, "the pig is still there");
    ctx.check(pig->getHealth() == health, "attacks are cancelled while carrying");

    hit.set(true);
    co_await ctx.ticks(12);
    pig = fetch(pigId);
    ctx.require(pig != nullptr, "the pig is still there");
    sim.simulateLookAt(*pig);
    sim.simulateAttack(pig);
    co_await ctx.ticks(2);
    pig = fetch(pigId);
    ctx.check(!pig || pig->getHealth() < health, "hitWhileCarrying=true allows attacks");
    if (pig) pig->remove();
});

LO_REGISTER_SELFTEST("restrictions.destroy", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(630);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    p.setPlayerGameType(GameType::Survival);
    BlockPos target{feet.x + 1, feet.y, feet.z};
    auto&    region = p.getDimensionBlockSource();
    ctx.require(ctx.setBlock(lo::game::globalPos(region, target), "minecraft:dirt"), "dirt placed");
    co_await ctx.ticks(2);
    auto const* dirt = lo::game::blockWith("minecraft:dirt");
    ctx.require(dirt != nullptr, "dirt exists");

    CarryGuard   guard(p);
    SettingGuard hit(&carry::CarryConfig::Settings::hitWhileCarrying, false);
    startCarry(p);
    co_await ctx.ticks(1);
    ctx.check(p.getDestroyProgress(*dirt) == 0.0f, "destroy progress is 0 while carrying");

    {
        ll::event::PlayerDestroyBlockEvent ev(p, target);
        ll::event::EventBus::getInstance().publish(ev);
        ctx.check(ev.isCancelled(), "PlayerDestroyBlockEvent is cancelled while carrying");
    }
    // The spine's synthetic protection event (block pickup) is not U6's business.
    carry::PickupTarget t{p, region, dirt, &target};
    ctx.check(carry::protectionAllows(t), "protection events published by the spine are ignored");

    auto& sim = static_cast<SimulatedPlayer&>(p);
    sim.simulateDestroyBlock(target, ScriptFacing::Up);
    co_await ctx.ticks(40);
    sim.simulateStopDestroyingBlock();
    ctx.check(lo::game::blockIdAt(region, target) == "minecraft:dirt", "the block is not broken while carrying");

    hit.set(true);
    ctx.check(p.getDestroyProgress(*dirt) > 0.0f, "hitWhileCarrying=true: normal destroy progress");
    {
        ll::event::PlayerDestroyBlockEvent ev(p, target);
        ll::event::EventBus::getInstance().publish(ev);
        ctx.check(!ev.isCancelled(), "hitWhileCarrying=true: breaking is allowed");
    }
    sim.simulateDestroyBlock(target, ScriptFacing::Up);
    for (int i = 0; i < 20 && lo::game::blockIdAt(region, target) == "minecraft:dirt"; ++i) co_await ctx.ticks(5);
    sim.simulateStopDestroyingBlock();
    ctx.check(lo::game::blockIdAt(region, target) != "minecraft:dirt", "hitWhileCarrying=true: the block breaks");
});

LO_REGISTER_SELFTEST("restrictions.container", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(640);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    BlockPos chest{feet.x + 1, feet.y, feet.z};
    auto&    region = p.getDimensionBlockSource();
    ctx.require(ctx.setBlock(lo::game::globalPos(region, chest), "minecraft:chest"), "chest placed");
    co_await ctx.ticks(2);
    CarryGuard guard(p);
    auto&      sim = static_cast<SimulatedPlayer&>(p);

    sim.simulateLookAt(chest, sim::LookDuration::Instant);
    sim.simulateInteract(chest, ScriptFacing::Up);
    co_await ctx.ticks(2);
    ctx.require(hasContainerOpen(p), "the chest screen opened (not carrying)");

    startCarry(p);
    co_await ctx.ticks(2);
    ctx.check(!hasContainerOpen(p), "an open container is closed while carrying");

    static_cast<ServerPlayer&>(p).openInventory();
    co_await ctx.ticks(1);
    ctx.check(!hasContainerOpen(p), "the inventory does not open while carrying");
});

LO_REGISTER_SELFTEST("restrictions.drop_when_hit", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(650);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(2);
    CarryGuard   guard(p);
    SettingGuard drop(&carry::CarryConfig::Settings::dropCarriedWhenHit, false);
    auto&        region = p.getDimensionBlockSource();

    // Off (default): damage keeps the carry.
    startCarry(p, R"({name:"minecraft:sponge"})");
    co_await ctx.ticks(1);
    p.hurtByCause(1.0f, SharedTypes::Legacy::ActorDamageCause::Magic);
    co_await ctx.ticks(3);
    ctx.check(carry::isCarrying(p), "dropCarriedWhenHit=false: damage keeps the carry");

    // On: the carry is placed near the carrier.
    drop.set(true);
    co_await ctx.ticks(12); // past the hurt cooldown
    p.hurtByCause(1.0f, SharedTypes::Legacy::ActorDamageCause::Magic);
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p), "dropCarriedWhenHit=true: damage places the carry");
    bool found = false;
    for (int dx = -3; dx <= 3; ++dx) {
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dz = -3; dz <= 3; ++dz) {
                BlockPos at{feet.x + dx, feet.y + dy, feet.z + dz};
                if (lo::game::blockIdAt(region, at) == "minecraft:sponge") {
                    found = true;
                    ctx.cleanupBlock(lo::game::globalPos(region, at));
                }
            }
        }
    }
    ctx.check(found, "the carried sponge was placed next to the carrier");
});

/// For the client e2e scenario (a real player in a local world). Without a real player (BDS, server
/// harness) it does nothing. With the tag `lo_e2e_restrictions` it keeps the real player carrying until
/// the tag is removed (up to 90 s) so the scenario can press keys; otherwise it checks the snap-back.
LO_REGISTER_SELFTEST("restrictions.real_player", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto level = ll::service::getLevel();
    ctx.require(static_cast<bool>(level), "a level");
    Player* real = nullptr;
    level->forEachPlayer([&](Player& p) {
        if (!p.isSimulatedPlayer()) {
            real = &p;
            return false;
        }
        return true;
    });
    if (!real) {
        ctx.info("no real player online; nothing to check");
        co_return;
    }
    std::int64_t id          = real->getOrCreateUniqueID().rawID;
    bool         interactive = real->hasTag("lo_e2e_restrictions");
    if (carry::isCarrying(*real)) {
        ctx.info("the real player already carries something; left alone");
        co_return;
    }
    CarryGuard guard(*real); // only after the check: never clear a real carry
    startCarry(*real);
    int locked = carry::get(*real).selected;
    lo::log().info("restrictions: e2e carry ready on {} (slot {})", real->getRealName(), locked);

    if (interactive) {
        for (int i = 0; i < 360; ++i) {
            co_await ctx.ticks(5);
            auto* p = level->getPlayer(ActorUniqueID(id));
            if (!p || !p->hasTag("lo_e2e_restrictions")) break;
        }
    } else {
        real->setSelectedSlot((locked + 1) % 9);
        co_await ctx.ticks(2);
    }
    auto* p = level->getPlayer(ActorUniqueID(id));
    ctx.require(p != nullptr, "the real player is still online");
    ctx.check(p->getSelectedItemSlot() == locked, "the real player's hotbar slot stayed locked");
    ctx.check(!hasContainerOpen(*p), "no container screen open while carrying");
    lo::log().info("restrictions: e2e carry done on {} (slot {})", p->getRealName(), p->getSelectedItemSlot());
});
