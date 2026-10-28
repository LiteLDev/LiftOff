// Selftests of U4 entityplace (docs/SPEC.md §3.7, §3.8). dx 400..499.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components_json_legacy/RideableComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/HandSlot.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "feature/entityplace/EntityPlace.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace ep    = lo::feature::entityplace;
using carry::CarryData;
using carry::CarryType;
using ep::StackResult;

BlockPos offset(BlockPos const& p, int dx, int dy, int dz) { return {p.x + dx, p.y + dy, p.z + dz}; }
lo::core::GlobalPos gpos(Context& ctx, BlockPos const& p) {
    return lo::game::globalPos(ctx.player().getDimensionBlockSource(), p);
}

/// Puts the test player at `pos` (feet).
void standAt(Player& p, BlockPos const& pos) {
    p.teleport(Vec3{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f}, p.getDimensionId());
}

/// Spawns an adult `id` (spawn events make some mobs babies at random; babies have other sizes and
/// no seats).
Actor* spawnMob(Player& p, std::string const& id, BlockPos const& at) {
    auto level = ll::service::getLevel();
    for (int attempt = 0; attempt < 20; ++attempt) {
        Actor* a = level->getSpawner().spawnMob(p.getDimensionBlockSource(), ActorDefinitionIdentifier(id), nullptr, at.bottomCenter(), false, false, false);
        if (!a || !a->isBaby()) return a;
        a->remove();
    }
    return nullptr;
}

Actor* fetch(std::int64_t id) { return ll::service::getLevel()->fetchEntity(ActorUniqueID{id}, false); }

/// Spawns `id` at `at`, picks it up into `p`'s carry (like U3) and returns its unique id (0 on failure).
std::int64_t carryMob(Player& p, std::string const& id, BlockPos const& at, std::string const& name) {
    Actor* mob = spawnMob(p, id, at);
    if (!mob) return 0;
    mob->setNameTag(name);
    auto uid = mob->getOrCreateUniqueID().rawID;
    CarryData d;
    d.type   = CarryType::Entity;
    d.entity = carry::captureActor(*mob);
    carry::removeCapturedActor(*mob);
    carry::set(p, d);
    carry::applySlowness(p, carry::entityAmplifier(*mob, p));
    return uid;
}

void removeActor(std::int64_t id) {
    if (auto* a = fetch(id)) {
        ep::stopEmulatedRide(*a);
        a->remove();
    }
}

/// Restores the carry config after a test changed it.
struct ConfigGuard {
    carry::CarryConfig saved = lo::config::get<carry::CarryConfig>();
    ~ConfigGuard() {
        lo::config::mut<carry::CarryConfig>() = saved;
        carry::rebuildLists();
    }
};

/// Common start: the player stands at `base` on a stone floor with air above.
ll::coro::CoroTask<bool> prepare(Context& ctx, BlockPos const& base) {
    if (!co_await ctx.ensureLoaded(gpos(ctx, base))) co_return false;
    for (int dx = -1; dx <= 4; ++dx)
        for (int dz = -1; dz <= 1; ++dz) {
            ctx.setBlock(gpos(ctx, offset(base, dx, -1, dz)), "minecraft:stone");
            for (int dy = 0; dy <= 3; ++dy) ctx.setBlock(gpos(ctx, offset(base, dx, dy, dz)), "minecraft:air");
        }
    standAt(ctx.player(), base);
    co_await ctx.ticks(2);
    co_return true;
}
} // namespace

// Right-click on the top of a block: the pig appears on it with its unique id and data.
LO_REGISTER_SELFTEST("entityplace.place_floor", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(400));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    BlockPos stone = offset(base, 2, 0, 0);
    ctx.setBlock(gpos(ctx, stone), "minecraft:stone");
    auto uid = carryMob(p, "minecraft:pig", offset(base, 3, 1, 0), "lo_ep_pig");
    ctx.require(uid != 0 && carry::get(p).isEntity(), "carrying a pig");
    co_await ctx.ticks(2);

    // The server path of a client's use-block with an empty hand (ItemUseInventoryTransaction).
    ItemStack hand;
    (void)p.mGameMode->useItemOn(hand, stone, 1, stone.center(), HandSlot::Mainhand, &p.getDimensionBlockSource().getBlock(stone), true);
    ctx.check(carry::get(p).isEntity(), "placement waits for the next tick");
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness is removed");
    Actor* pig = fetch(uid);
    ctx.require(pig != nullptr, "the pig exists again with the same unique id");
    ctx.check(pig->getNameTag() == "lo_ep_pig", "the name tag is kept");
    Vec3 want = offset(stone, 0, 1, 0).bottomCenter();
    ctx.check(pig->getPosition().distanceTo(want) < 0.6f, "placed at the bottom centre above the clicked block");
    removeActor(uid);
});

// Right-click on the side of a block: the pig appears next to it.
LO_REGISTER_SELFTEST("entityplace.place_wall", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(410));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    BlockPos wall = offset(base, 2, 1, 0);
    ctx.setBlock(gpos(ctx, wall), "minecraft:stone");
    auto uid = carryMob(p, "minecraft:pig", offset(base, 2, 0, 1), "lo_ep_wall");
    ctx.require(uid != 0, "carrying a pig");
    co_await ctx.ticks(2);
    ctx.check(ep::tryPlaceEntity(p, wall, 4 /*west*/), "placed on the west side");
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    Actor* pig = fetch(uid);
    ctx.require(pig != nullptr, "the pig exists");
    ctx.check(pig->getPosition().distanceTo(offset(wall, -1, 0, 0).bottomCenter()) < 0.6f, "placed west of the wall block");
    removeActor(uid);
});

// Neither the clicked block nor its neighbour is replaceable: lava pop, the carry is kept.
LO_REGISTER_SELFTEST("entityplace.place_blocked", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(420));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    BlockPos a = offset(base, 2, 0, 0);
    ctx.setBlock(gpos(ctx, a), "minecraft:stone");
    ctx.setBlock(gpos(ctx, offset(a, 0, 1, 0)), "minecraft:stone");
    auto uid = carryMob(p, "minecraft:pig", offset(base, 3, 0, 1), "lo_ep_blocked");
    ctx.require(uid != 0, "carrying a pig");
    co_await ctx.ticks(2);
    ctx.check(!ep::tryPlaceEntity(p, a, 1), "nothing is placed");
    ctx.check(carry::get(p).isEntity(), "the carry is kept");
    ctx.check(fetch(uid) == nullptr, "no pig appeared");

    // Use-block while carrying an entity is always denied (the clicked block is never used).
    ItemStack empty;
    Vec3      click = a.center();
    FacingID  face  = FacingID::Up;
    ll::event::PlayerInteractBlockEvent ev(p, empty, a, face, click, p.getDimensionBlockSource().getBlock(a));
    ll::event::EventBus::getInstance().publish(ev);
    ctx.check(ev.isCancelled(), "use-block is cancelled while carrying an entity");
    co_await ctx.ticks(2);
    ctx.check(carry::get(p).isEntity(), "the deferred placement failed too, the carry is kept");
    carry::clear(p);
    carry::removeSlowness(p);
});

// A pig is stacked on a cow (the cow has no seat on Bedrock: emulated link).
LO_REGISTER_SELFTEST("entityplace.stack_pig_on_cow", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(430));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    Actor* cow = spawnMob(p, "minecraft:cow", offset(base, 1, 0, 0));
    ctx.require(cow != nullptr, "a cow spawned");
    auto cowId = cow->getOrCreateUniqueID().rawID;
    auto uid   = carryMob(p, "minecraft:pig", offset(base, 3, 0, 1), "lo_ep_stack");
    ctx.require(uid != 0, "carrying a pig");
    co_await ctx.ticks(2);
    cow = fetch(cowId);
    ctx.require(cow != nullptr, "the cow is still there");

    // The server path of a client's entity interact (ItemUseOnActorInventoryTransaction).
    ctx.check(!p.mGameMode->interact(*cow, cow->getPosition(), HandSlot::Mainhand), "the vanilla interaction is cancelled");
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness is removed");
    Actor* pig = fetch(uid);
    cow        = fetch(cowId);
    ctx.require(pig && cow, "pig and cow exist");
    ctx.check(ep::vehicleOf(*pig) == cow, "the pig rides the cow");
    ctx.check(ep::firstPassengerOf(*cow) == pig, "the cow carries the pig");
    ctx.check(&ep::rootVehicleOf(*pig) == cow, "the cow is the root of the column");
    float top = cow->getFeetPos().y + carry::entityHeight(*cow);
    ctx.check(std::abs(pig->getFeetPos().y - top) < 0.75f, "the pig sits on top of the cow");
    ctx.info(std::string("link: ") + (pig->isRiding(cow) ? "real" : "emulated"));
    co_await ctx.ticks(10);
    pig = fetch(uid);
    cow = fetch(cowId);
    ctx.check(pig && cow && ep::vehicleOf(*pig) == cow, "the pig still rides the cow later");

    // Picking the cow up (U3) ends the link: the pig is no longer kept on it.
    if (cow) {
        removeActor(cowId);
        co_await ctx.ticks(2);
        pig = fetch(uid);
        ctx.check(pig && ep::vehicleOf(*pig) == nullptr, "the link ends with the vehicle");
    }
    removeActor(uid);
});

// The held entity must not be bigger than the top one; the carry is kept.
LO_REGISTER_SELFTEST("entityplace.stack_size", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(440));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    Actor* chicken = spawnMob(p, "minecraft:chicken", offset(base, 1, 0, 0));
    ctx.require(chicken != nullptr, "a chicken spawned");
    auto chickenId = chicken->getOrCreateUniqueID().rawID;
    auto uid       = carryMob(p, "minecraft:cow", offset(base, 3, 0, 1), "lo_ep_big");
    ctx.require(uid != 0, "carrying a cow");
    co_await ctx.ticks(2);
    chicken = fetch(chickenId);
    ctx.require(chicken != nullptr, "the chicken is still there");
    ctx.check(ep::tryStack(p, *chicken) == StackResult::TooBig, "a cow is too big for a chicken");
    ctx.check(carry::get(p).isEntity(), "the carry is kept");
    ctx.check(fetch(uid) == nullptr, "the cow did not appear");
    {
        ConfigGuard guard;
        lo::config::mut<carry::CarryConfig>().settings.entitySizeMattersStacking = false;
        ctx.check(ep::tryStack(p, *chicken) == StackResult::Stacked, "size ignored when entitySizeMattersStacking is off");
    }
    ctx.check(!carry::isCarrying(p), "the carry is cleared after stacking");
    removeActor(uid);
    removeActor(chickenId);
    carry::removeSlowness(p);
});

// The column is limited to maxEntityStackLimit entities.
LO_REGISTER_SELFTEST("entityplace.stack_limit", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(450));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    ConfigGuard guard;
    auto&       s = lo::config::mut<carry::CarryConfig>().settings;
    s.maxEntityStackLimit       = 3; // passengers above the root must stay < 2
    s.entitySizeMattersStacking = false;
    Actor* cow = spawnMob(p, "minecraft:cow", offset(base, 1, 0, 0));
    ctx.require(cow != nullptr, "a cow spawned");
    auto cowId = cow->getOrCreateUniqueID().rawID;
    std::int64_t ids[3]{};
    for (int i = 0; i < 3; ++i) {
        ids[i] = carryMob(p, "minecraft:pig", offset(base, 3, 0, 1), "lo_ep_limit" + std::to_string(i));
        ctx.require(ids[i] != 0, "carrying a pig");
        co_await ctx.ticks(2);
        cow = fetch(cowId);
        ctx.require(cow != nullptr, "the cow is there");
        auto r = ep::tryStack(p, *cow);
        if (i < 2) ctx.check(r == StackResult::Stacked, "pig " + std::to_string(i) + " is stacked");
        else ctx.check(r == StackResult::Full, "the third pig does not fit (limit 3)");
    }
    ctx.check(carry::get(p).isEntity(), "the last pig is still carried");
    Actor* first = fetch(ids[0]);
    Actor* second = fetch(ids[1]);
    ctx.check(first && second && ep::vehicleOf(*second) == first, "the second pig sits on the first");
    carry::clear(p);
    carry::removeSlowness(p);
    for (auto id : ids) removeActor(id);
    removeActor(cowId);
});

// Horses are in the default stacking blacklist; in whitelist mode a horse at the top gets tamed.
LO_REGISTER_SELFTEST("entityplace.stack_forbidden", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(460));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    Actor* horse = spawnMob(p, "minecraft:horse", offset(base, 1, 0, 0));
    ctx.require(horse != nullptr, "a horse spawned");
    auto horseId = horse->getOrCreateUniqueID().rawID;
    auto uid     = carryMob(p, "minecraft:pig", offset(base, 3, 0, 1), "lo_ep_horse");
    ctx.require(uid != 0, "carrying a pig");
    co_await ctx.ticks(2);
    horse = fetch(horseId);
    ctx.require(horse != nullptr, "the horse is there");
    ctx.check(ep::tryStack(p, *horse) == StackResult::Forbidden, "stacking on a horse is forbidden");
    ctx.check(carry::get(p).isEntity(), "the carry is kept");
    {
        ConfigGuard guard;
        auto&       cfg                  = lo::config::mut<carry::CarryConfig>();
        cfg.settings.useWhitelistStacking = true;
        cfg.whitelist.allowedStacking     = {"minecraft:horse"};
        carry::rebuildLists();
        ctx.check(!horse->isTame(), "the horse is wild");
        ctx.check(ep::tryStack(p, *horse) == StackResult::Stacked, "whitelisted horse accepts the pig");
        co_await ctx.ticks(2);
        horse = fetch(horseId);
        ctx.check(horse && horse->isTame(), "the horse is tamed");
        Actor* pig = fetch(uid);
        ctx.check(pig && horse && ep::vehicleOf(*pig) == horse, "the pig rides the horse");
        ctx.info(std::string("horse link: ") + (pig && horse && pig->isRiding(horse) ? "real" : "emulated"));
    }
    removeActor(uid);
    removeActor(horseId);
});

// Stacking is off with stackableEntities false; stacking needs an entity carry.
LO_REGISTER_SELFTEST("entityplace.stack_disabled", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(470));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    Actor* cow = spawnMob(p, "minecraft:cow", offset(base, 1, 0, 0));
    ctx.require(cow != nullptr, "a cow spawned");
    auto cowId = cow->getOrCreateUniqueID().rawID;
    ctx.check(ep::tryStack(p, *cow) == StackResult::NotCarrying, "nothing carried, nothing stacked");
    auto uid = carryMob(p, "minecraft:pig", offset(base, 3, 0, 1), "lo_ep_off");
    co_await ctx.ticks(2);
    cow = fetch(cowId);
    ctx.require(uid != 0 && cow != nullptr, "carrying a pig next to a cow");
    {
        ConfigGuard guard;
        lo::config::mut<carry::CarryConfig>().settings.stackableEntities = false;
        ctx.check(ep::tryStack(p, *cow) == StackResult::Disabled, "stacking disabled by config");
        ctx.check(carry::get(p).isEntity(), "the carry is kept");
    }
    carry::clear(p);
    carry::removeSlowness(p);
    removeActor(cowId);
});

// A vehicle without seats on Bedrock gets an emulated link: the rider is kept on top of it.
LO_REGISTER_SELFTEST("entityplace.ride_emulated", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&    p    = ctx.player();
    BlockPos base = lo::game::blockPos(ctx.testPos(480));
    ctx.require(co_await prepare(ctx, base), "test area ready");
    std::int64_t vehicleId = 0;
    for (char const* id : {"minecraft:villager_v2", "minecraft:sheep", "minecraft:iron_golem", "minecraft:wolf", "minecraft:cat",
                           "minecraft:armadillo", "minecraft:goat", "minecraft:panda", "minecraft:turtle"}) {
        Actor* a = spawnMob(p, id, offset(base, 2, 0, 0));
        if (!a) continue;
        if (!a->getEntityContext().hasComponent<RideableComponent>()) {
            vehicleId = a->getOrCreateUniqueID().rawID;
            ctx.info(std::string("seatless vehicle: ") + id);
            break;
        }
        a->remove();
    }
    if (vehicleId == 0) {
        ctx.info("every candidate has seats; emulation not exercised");
        co_return;
    }
    Actor* rider = spawnMob(p, "minecraft:chicken", offset(base, 3, 0, 1));
    Actor* vehicle = fetch(vehicleId);
    ctx.require(rider && vehicle, "rider and vehicle spawned");
    auto riderId = rider->getOrCreateUniqueID().rawID;
    ctx.check(ep::ride(*rider, *vehicle), "ride() succeeds");
    ctx.check(ep::isEmulatedRide(*rider, *vehicle), "the link is emulated");
    co_await ctx.ticks(20);
    rider   = fetch(riderId);
    vehicle = fetch(vehicleId);
    ctx.require(rider && vehicle, "rider and vehicle still exist");
    ctx.check(ep::vehicleOf(*rider) == vehicle && ep::firstPassengerOf(*vehicle) == rider, "the link holds");
    float top = vehicle->getFeetPos().y + carry::entityHeight(*vehicle);
    ctx.check(std::abs(rider->getFeetPos().y - top) < 0.2f, "the rider stays on top of the vehicle");
    ctx.check(rider->getFeetPos().distanceTo(vehicle->getFeetPos()) < carry::entityHeight(*vehicle) + 0.5f, "and above it");
    // Picking the rider up ends the link (it does not snap back when placed again).
    CarryData d;
    d.type   = CarryType::Entity;
    d.entity = carry::captureActor(*rider);
    carry::removeCapturedActor(*rider);
    carry::set(p, d);
    co_await ctx.ticks(2);
    BlockPos away = offset(base, -1, 0, 1);
    ctx.check(ep::tryPlaceEntity(p, offset(away, 0, -1, 0), 1), "the chicken is placed elsewhere");
    co_await ctx.ticks(5);
    rider = fetch(riderId);
    ctx.require(rider != nullptr, "the chicken is back");
    ctx.check(ep::vehicleOf(*rider) == nullptr, "and rides nothing");
    removeActor(riderId);
    removeActor(vehicleId);
});
