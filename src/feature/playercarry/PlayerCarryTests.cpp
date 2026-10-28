// Selftests of carrying players (U5, dx 500..599).

#include <cmath>
#include <format>
#include <utility>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"

#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Slowness.h"
#include "feature/playercarry/PlayerCarry.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace pc    = lo::feature::playercarry;
using carry::CarryType;

constexpr float kPi = 3.14159265358979f;

/// Feet position at the centre of block `pos`.
Vec3 centre(lo::core::GlobalPos const& pos, float dx = 0.f, float dz = 0.f) {
    return Vec3{static_cast<float>(pos.x) + 0.5f + dx, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f + dz};
}

struct Pair {
    Player&          carrier;
    SimulatedPlayer& target;
};

/// The test player (carrier) at the test position facing south, a second player one block south.
/// `untracked`: the second player is not removed by the context (the test disconnects it itself).
ll::coro::CoroTask<Pair> setup(Context& ctx, int dx, bool untracked = false) {
    auto pos = ctx.testPos(dx);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto&            carrier = ctx.player();
    SimulatedPlayer* spawned = nullptr;
    if (untracked) {
        auto sp = SimulatedPlayer::create("lo_pc_" + std::to_string(dx), centre(pos, 0.f, 1.f), 0);
        ctx.require(static_cast<bool>(sp), "second player spawned");
        spawned = sp.as_ptr();
    }
    auto& target = untracked ? *spawned : ctx.spawnPlayer();
    carrier.teleport(centre(pos), carrier.getDimensionId(), Vec2{0.f, 0.f});
    target.teleport(centre(pos, 0.f, 1.f), carrier.getDimensionId(), Vec2{0.f, 180.f});
    co_await ctx.ticks(5);
    carry::setKeyPressed(carrier, true);
    co_return Pair{carrier, target};
}

void finish(Pair& p) {
    carry::resetKeyPressed(p.carrier);
    pc::release(p.carrier, "test end");
    carry::clear(p.carrier);
    carry::removeSlowness(p.carrier);
}

/// Restores a config value when the test ends.
template <class T>
struct Restore {
    T& ref;
    T  old;
    explicit Restore(T& r, T v) : ref(r), old(r) { ref = v; }
    ~Restore() { ref = old; }
};

auto& settings() { return lo::config::mut<carry::CarryConfig>().settings; }

std::string vecText(Vec3 const& v) { return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z); }

/// The rider's feet relative to the carrier's feet, in the carrier's frame: (right, up, forward).
Vec3 localOffset(Player& carrier, Player& rider) {
    Vec3  d   = rider.getFeetPos() - carrier.getFeetPos();
    float yaw = carrier.getRotation().y * kPi / 180.f;
    // Bedrock: yaw 0 looks south (+z); forward = (-sin, 0, cos).
    float fwd   = -std::sin(yaw) * d.x + std::cos(yaw) * d.z;
    float right = -std::cos(yaw) * d.x - std::sin(yaw) * d.z;
    return Vec3{right, d.y, fwd};
}
} // namespace

LO_REGISTER_SELFTEST("playercarry.pickup", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 500);
    ctx.require(pc::tryPickup(p.carrier, p.target), "the player is picked up");
    ctx.check(carry::get(p.carrier).type == CarryType::Player, "carry type is Player");
    ctx.check(carry::get(p.carrier).player == p.target.getUuid().asString(), "the carry names the target");
    ctx.check(p.target.isRiding(&p.carrier), "the target rides the carrier");
    ctx.check(carry::slownessAmplifier(p.carrier) == 1, "Slowness II");
    ctx.check(pc::carrierOf(p.target) == &p.carrier, "carrierOf finds the carrier");
    ctx.check(pc::carriedPlayer(p.carrier) == &p.target, "carriedPlayer finds the target");
    co_await ctx.ticks(10);
    ctx.check(p.target.isRiding(&p.carrier), "still riding after 10 ticks");
    ctx.check(carry::get(p.carrier).isPlayer(), "still carrying after 10 ticks");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.interact", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 505);
    // Through the game's own entity interaction (the hook).
    p.carrier.mGameMode->interact(p.target, p.target.getFeetPos(), HandSlot::Mainhand);
    ctx.check(carry::get(p.carrier).isPlayer(), "interacting picks the player up");
    ctx.check(p.target.isRiding(&p.carrier), "the target rides the carrier");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.position", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 510);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    for (float yaw : {0.f, 90.f, -135.f}) {
        p.carrier.teleport(centre(ctx.testPos(510)), p.carrier.getDimensionId(), Vec2{0.f, yaw});
        static_cast<Mob&>(p.carrier).setYBodyRotation(yaw);
        co_await ctx.ticks(5);
        ctx.require(p.target.isRiding(&p.carrier), "riding after the carrier turned");
        auto off = localOffset(p.carrier, p.target);
        ctx.info(std::format("yaw {} offset (right, up, forward) {}", yaw, vecText(off)));
        ctx.check(std::abs(off.y - pc::kUp) < 0.05f, std::format("0.4 above the carrier's feet (yaw {}): {:.3f}", yaw, off.y));
        ctx.check(
            std::abs(off.z - pc::kForward) < 0.05f,
            std::format("0.6 in front of the carrier (yaw {}): {:.3f}", yaw, off.z)
        );
        ctx.check(std::abs(off.x) < 0.05f, std::format("centred (yaw {}): {:.3f}", yaw, off.x));
    }
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.yaw", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 515);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    p.carrier.teleport(centre(ctx.testPos(515)), p.carrier.getDimensionId(), Vec2{0.f, 0.f});
    co_await ctx.ticks(2);
    const_cast<Vec2&>(p.target.getRotation()).y = 120.f;
    p.target.setYHeadRot(120.f);
    co_await ctx.ticks(2);
    float carrierYaw = p.carrier.getRotation().y;
    float riderYaw   = p.target.getRotation().y;
    float diff       = std::remainder(riderYaw - carrierYaw, 360.f);
    ctx.info(std::format("carrier yaw {:.1f}, rider yaw {:.1f}", carrierYaw, riderYaw));
    ctx.check(std::abs(diff) <= pc::kYawLimitDeg + 0.5f, std::format("rider yaw within 30 degrees: {:.1f}", diff));
    float body = static_cast<Mob&>(p.target).getYBodyRotation();
    ctx.check(std::abs(std::remainder(body - carrierYaw, 360.f)) < 1.f, std::format("rider body yaw = carrier yaw: {:.1f}", body));
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.refusals", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 520);
    {
        Restore r(settings().pickupPlayers, false);
        ctx.check(!pc::tryPickup(p.carrier, p.target), "pickupPlayers false refuses");
    }
    p.target.setPlayerGameType(GameType::Creative);
    co_await ctx.ticks(1);
    ctx.check(!pc::tryPickup(p.carrier, p.target), "a survival carrier cannot pick up a creative player");
    p.target.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(1);
    carry::resetKeyPressed(p.carrier);
    ctx.check(!pc::tryPickup(p.carrier, p.target), "without the carry key: refused");
    carry::setKeyPressed(p.carrier, true);
    p.target.teleport(centre(ctx.testPos(520), 0.f, 4.f), p.carrier.getDimensionId());
    co_await ctx.ticks(2);
    ctx.check(!pc::tryPickup(p.carrier, p.target), "too far (4 blocks): refused");
    p.target.teleport(centre(ctx.testPos(520), 0.f, 1.f), p.carrier.getDimensionId());
    co_await ctx.ticks(2);
    ctx.give(p.carrier, lo::game::makeItem("minecraft:stick", 1, 0), p.carrier.getSelectedItemSlot());
    ctx.check(!pc::tryPickup(p.carrier, p.target), "an item in the hand: refused");
    p.carrier.setSelectedItem(ItemStack::EMPTY_ITEM());
    ctx.check(!carry::isCarrying(p.carrier), "nothing carried after the refusals");
    ctx.check(pc::tryPickup(p.carrier, p.target), "allowed once every rule passes");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.creative", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 525);
    p.carrier.setPlayerGameType(GameType::Creative);
    p.target.setPlayerGameType(GameType::Creative);
    co_await ctx.ticks(1);
    ctx.check(pc::tryPickup(p.carrier, p.target), "a creative carrier picks up a creative player");
    finish(p);
    p.carrier.setPlayerGameType(GameType::Survival);
});

LO_REGISTER_SELFTEST("playercarry.dismount", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 530);
    lo::net::setTestModded(p.carrier, true);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    (void)lo::net::takeTestMessages(p.carrier);
    // The carried player leaves the ride (sneak dismount ends in stopRiding).
    p.target.stopRiding(true, false, false, false);
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p.carrier), "the carrier's carry is cleared");
    ctx.check(carry::slownessAmplifier(p.carrier) < 0, "Slowness removed");
    bool sawRide = false;
    for (auto& [name, payload] : lo::net::takeTestMessages(p.carrier)) {
        if (name.find("StartRidingPacket") != std::string::npos && name.find("Other") == std::string::npos && payload.value("ride", true) == false) sawRide = true;
    }
    ctx.check(sawRide, "the carrier got start_riding(false)");
    lo::net::setTestModded(p.carrier, false);
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.sneak", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 535);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    p.target.simulateSneaking();
    co_await ctx.ticks(10);
    p.target.simulateStopSneaking();
    ctx.info(std::format("after sneaking: riding {}, carrying {}", p.target.isRiding(&p.carrier), carry::isCarrying(p.carrier)));
    ctx.check(p.target.isRiding(&p.carrier) == carry::isCarrying(p.carrier), "carry state follows the ride");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.disconnect", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 540, /*untracked*/ true);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    p.target.simulateDisconnect();
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p.carrier), "the carrier is resolved when the carried player leaves");
    ctx.check(carry::slownessAmplifier(p.carrier) < 0, "Slowness removed");
    ctx.check(!p.carrier.getFirstPassenger(), "no passenger left");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.death", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 545);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    p.target.kill();
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p.carrier), "the carrier is resolved when the carried player dies");
    ctx.check(carry::slownessAmplifier(p.carrier) < 0, "Slowness removed");
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.place", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 550);
    lo::net::setTestModded(p.carrier, true);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    (void)lo::net::takeTestMessages(p.carrier);
    // Click the top of the ground block two blocks east of the carrier.
    auto     pos     = ctx.testPos(552);
    BlockPos ground{pos.x, pos.y - 1, pos.z};
    ctx.require(pc::tryPlace(p.carrier, ground, /*up*/ 1), "placed");
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p.carrier), "carry cleared");
    ctx.check(carry::slownessAmplifier(p.carrier) < 0, "Slowness removed");
    ctx.check(!p.target.getVehicle(), "the target no longer rides");
    auto feet = p.target.getFeetPos();
    ctx.info(std::format("placed at {}", vecText(feet)));
    ctx.check(
        std::abs(feet.x - (pos.x + 0.5f)) < 0.01f && std::abs(feet.z - (pos.z + 0.5f)) < 0.01f && std::abs(feet.y - pos.y) < 0.01f,
        "teleported to the bottom centre of the block above the clicked one"
    );
    bool sawOther = false;
    for (auto& [name, payload] : lo::net::takeTestMessages(p.carrier)) {
        if (name.find("StartRidingOtherPacket") != std::string::npos && payload.value("ride", true) == false) sawOther = true;
    }
    ctx.check(sawOther, "start_riding_other(false) broadcast");
    lo::net::setTestModded(p.carrier, false);
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.place_blocked", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 555);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    auto pos = ctx.testPos(557);
    ctx.setBlock(pos, "minecraft:stone");
    ctx.setBlock({pos.dim, pos.x, pos.y + 1, pos.z}, "minecraft:stone");
    // Clicking the side of a stone with stone behind the face: no free block.
    ctx.check(!pc::tryPlace(p.carrier, BlockPos{pos.x, pos.y, pos.z}, /*up*/ 1), "no free block: refused");
    ctx.check(carry::get(p.carrier).isPlayer(), "still carrying");
    finish(p);
});

Actor* spawnMob(Player& near, std::string const& id, Vec3 const& at) {
    auto* level = ll::service::getLevel().as_ptr();
    return level->getSpawner().spawnMob(near.getDimensionBlockSource(), ActorDefinitionIdentifier(id), nullptr, at, false, false, false);
}

LO_REGISTER_SELFTEST("playercarry.stack_refused", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 560);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    auto* horse = spawnMob(p.carrier, "minecraft:horse", centre(ctx.testPos(562)));
    auto* pig   = spawnMob(p.carrier, "minecraft:pig", centre(ctx.testPos(562), 0.f, 2.f));
    ctx.require(horse && pig, "mobs spawned");
    co_await ctx.ticks(2);
    // Horses are on the default stacking blacklist: nothing happens.
    ctx.check(pc::tryStack(p.carrier, *horse), "handled");
    ctx.check(carry::get(p.carrier).isPlayer(), "a blacklisted top entity leaves the carry");
    // A player (0.6 x 1.8) is larger than a pig (0.9 x 0.9): refused.
    ctx.check(pc::tryStack(p.carrier, *pig), "handled");
    ctx.check(carry::get(p.carrier).isPlayer(), "a smaller top entity leaves the carry");
    {
        Restore r(settings().stackableEntities, false);
        ctx.check(!pc::tryStack(p.carrier, *pig), "stackableEntities false: not handled");
    }
    horse->remove();
    pig->remove();
    finish(p);
});

LO_REGISTER_SELFTEST("playercarry.stack", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await setup(ctx, 565);
    ctx.require(pc::tryPickup(p.carrier, p.target), "picked up");
    co_await ctx.ticks(2);
    auto* cow = spawnMob(p.carrier, "minecraft:cow", centre(ctx.testPos(567)));
    ctx.require(cow != nullptr, "cow spawned");
    co_await ctx.ticks(2);
    ctx.check(pc::tryStack(p.carrier, *cow), "stacked");
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p.carrier), "carry cleared");
    ctx.check(carry::slownessAmplifier(p.carrier) < 0, "Slowness removed");
    ctx.check(!p.target.isRiding(&p.carrier), "off the carrier");
    ctx.check(p.target.isRiding(cow), "the target rides the cow");
    co_await ctx.ticks(10);
    ctx.check(p.target.isRiding(cow), "still riding the cow after 10 ticks");
    ctx.check((p.target.getFeetPos() - cow->getFeetPos()).length() < 2.5f, "next to the cow");
    p.target.stopRiding(true, false, false, false);
    cow->remove();
    finish(p);
});
