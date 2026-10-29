// Selftests of U10 (client build only; dx 1000-1099). The rendering tests give the real local
// player a carry on the integrated server, wait until the client drew it in first person, hold it
// for a few seconds (the e2e scenario takes a screenshot meanwhile) and clear it again. Without a
// real player (BDS, or the client in a remote world) they only run the transform checks.
#ifdef LO_CLIENT

#include <cmath>
#include <string>

#include "glm/vec4.hpp"

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"

#include "client/firstperson/FirstPerson.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace fp    = lo::client::firstperson;
using carry::CarryData;
using carry::CarryType;
using carry::CarryVisual;
using Kind = fp::DrawStatus::Kind;

/// Ticks the carry stays drawn after the first frame, so the e2e scenario can take a screenshot.
constexpr int kHoldTicks = 100;

bool near(glm::vec4 const& a, glm::vec3 const& b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f && std::abs(a.z - b.z) < 1e-3f;
}

/// The (only) real player of the integrated server, or nullptr.
Player* localPlayer() {
    auto    level = ll::service::getLevel();
    Player* found = nullptr;
    if (!level) return nullptr;
    level->forEachPlayer([&](Player& p) {
        if (!p.isSimulatedPlayer()) {
            found = &p;
            return false;
        }
        return true;
    });
    return found;
}

/// Gives the local player `data`, waits for the first-person renderer to draw `kind`/`id`, holds
/// it, clears it and waits until the renderer let it go.
ll::coro::CoroTask<> showCarry(Context& ctx, CarryData data, Kind kind, std::string id) {
    auto* p = localPlayer();
    if (!p) {
        ctx.info("no local player: rendering not checked");
        co_return;
    }
    auto uid = p->getOrCreateUniqueID();
    ctx.require(!carry::isCarrying(*p), "the local player carries nothing");
    carry::set(*p, std::move(data));
    bool drawn = false;
    for (int i = 0; i < 200 && !drawn; ++i) {
        co_await ctx.ticks(1);
        auto st = fp::status();
        drawn   = st.kind == kind && st.what == id && st.frames > 2;
    }
    auto st = fp::status();
    ctx.check(drawn, "drawn in first person (last: " + st.what + ", frames " + std::to_string(st.frames) + ")");
    if (kind == Kind::Entity) ctx.check(st.actorAlive, "the display actor exists");
    ctx.info("holding " + id);
    co_await ctx.ticks(kHoldTicks);
    auto level = ll::service::getLevel();
    if (auto* again = level ? level->getPlayer(uid) : nullptr) carry::clear(*again);
    bool released = false;
    for (int i = 0; i < 100 && !released; ++i) {
        co_await ctx.ticks(1);
        auto now = fp::status();
        released = now.kind == Kind::None && !now.actorAlive;
    }
    ctx.check(released, "the renderer released the carry");
}
} // namespace

LO_REGISTER_SELFTEST("firstperson.is_chest", [](Context& ctx) {
    for (auto id :
         {"minecraft:chest",
          "minecraft:trapped_chest",
          "minecraft:ender_chest",
          "minecraft:copper_chest",
          "minecraft:waxed_oxidized_copper_chest"}) {
        ctx.check(fp::isChest(id), std::string(id) + " is a chest");
    }
    for (auto id : {"minecraft:barrel", "minecraft:furnace", "minecraft:chest_minecart", "minecraft:shulker_box"}) {
        ctx.check(!fp::isChest(id), std::string(id) + " is not a chest");
    }
});

LO_REGISTER_SELFTEST("firstperson.transforms", [](Context& ctx) {
    CarryVisual v;
    // Block: the model centre sits at 2.5 * (0, -0.5, -1).
    auto b = fp::blockTransform(false, false, v);
    ctx.check(near(b * glm::vec4{0, 0, 0, 1}, {0.f, -1.25f, -2.5f}), "block centre");
    // Not facing the player (plain block): tilted +8 degrees about X, so +Y leans towards -Z... and
    // the front (+Z of the model) still faces the camera.
    auto front = b * glm::vec4{0, 0, 1, 0};
    ctx.check(front.z > 2.4f, "a plain block shows its front (+Z) to the camera");
    // Chests (and facePlayer blocks) are turned 180 degrees.
    auto c = fp::blockTransform(false, true, v);
    ctx.check((c * glm::vec4{0, 0, 1, 0}).z < -2.4f, "a chest is turned around");
    ctx.check((fp::blockTransform(true, false, v) * glm::vec4{0, 0, 1, 0}).z < -2.4f, "facePlayer turns blocks");
    ctx.check((fp::blockTransform(true, true, v) * glm::vec4{0, 0, 1, 0}).z > 2.4f, "facePlayer keeps chests");
    // Entity (pig 0.9 x 0.9): feet at 0.8 * (0, -1.1, -(0.9 * 1.3 + 0.1)) after the 180 degree turn.
    auto e = fp::entityTransform(0.9f, 0.9f, false, v);
    ctx.check(near(e * glm::vec4{0, 0, 0, 1}, {0.f, -0.88f, -0.8f * 1.27f}), "entity feet");
    auto side = fp::entityTransform(0.9f, 0.9f, true, v);
    ctx.check(std::abs((side * glm::vec4{0, 0, 1, 0}).x) > 0.79f, "rotateEntitiesSideways turns 90 degrees");
    // Script transform: translation applied in the rotated frame, then the scale.
    v.translation = {0.f, 1.f, 0.f};
    v.scale       = {2.f, 2.f, 2.f};
    auto s        = fp::blockTransform(false, false, v);
    ctx.check(std::abs((s * glm::vec4{1, 0, 0, 0}).x - 5.f) < 1e-3f, "script scale");
    ctx.check((s * glm::vec4{0, 0, 0, 1}).y > -1.25f + 2.4f, "script translation");
});

LO_REGISTER_SELFTEST("firstperson.chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    if (!localPlayer()) {
        ctx.info("no local player: skipped");
        co_return;
    }
    auto pos = ctx.testPos(1000);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:chest"), "chest placed");
    co_await ctx.ticks(1);
    auto& region = ctx.player().getDimensionBlockSource();
    auto  bp     = lo::game::blockPos(pos);
    if (auto box = region.tryGetContainer(bp)) {
        box->setItem(0, lo::game::makeItem("minecraft:diamond", 7));
        box->setItem(4, lo::game::makeItem("minecraft:stick", 32));
    }
    auto cap = carry::captureBlock(region, bp);
    ctx.require(cap.has_value(), "chest captured");
    if (auto box = region.tryGetContainer(bp)) box->removeAllItems(); // no drops at cleanup
    co_await showCarry(
        ctx,
        CarryData{.type = CarryType::Block, .block = cap->block, .blockEntity = cap->blockEntity},
        Kind::Block,
        "minecraft:chest"
    );
});

LO_REGISTER_SELFTEST("firstperson.furnace", [](Context& ctx) -> ll::coro::CoroTask<> {
    if (!localPlayer()) {
        ctx.info("no local player: skipped");
        co_return;
    }
    auto pos = ctx.testPos(1010);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:furnace"), "furnace placed");
    co_await ctx.ticks(1);
    auto cap = carry::captureBlock(ctx.player().getDimensionBlockSource(), lo::game::blockPos(pos));
    ctx.require(cap.has_value(), "furnace captured");
    co_await showCarry(
        ctx,
        CarryData{.type = CarryType::Block, .block = cap->block, .blockEntity = cap->blockEntity},
        Kind::Block,
        "minecraft:furnace"
    );
});

LO_REGISTER_SELFTEST("firstperson.pig", [](Context& ctx) -> ll::coro::CoroTask<> {
    if (!localPlayer()) {
        ctx.info("no local player: skipped");
        co_return;
    }
    auto pos = ctx.testPos(1020);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = ctx.player().getDimensionBlockSource();
    auto* pig    = level->getSpawner().spawnMob(
        region,
        ActorDefinitionIdentifier("minecraft:pig"),
        nullptr,
        lo::game::blockPos(pos).bottomCenter(),
        false,
        false,
        false
    );
    ctx.require(pig != nullptr, "a pig spawned");
    pig->setNameTag("lo_fp_pig");
    co_await ctx.ticks(2);
    auto snbt = carry::captureActor(*pig);
    carry::removeCapturedActor(*pig);
    co_await showCarry(ctx, CarryData{.type = CarryType::Entity, .entity = snbt}, Kind::Entity, "minecraft:pig");
});

// Drawn over a wall right in front of the player (Java draws the hand layer after the world).
LO_REGISTER_SELFTEST("firstperson.pig_at_wall", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto* p = localPlayer();
    if (!p) {
        ctx.info("no local player: skipped");
        co_return;
    }
    auto  feet = p->getFeetBlockPos();
    float yaw  = glm::radians(p->getRotation().y);
    int   dx   = static_cast<int>(std::round(-std::sin(yaw))), dz = static_cast<int>(std::round(std::cos(yaw)));
    int   dim  = static_cast<int>(p->getDimensionId());
    for (int dy : {0, 1}) {
        ctx.require(
            ctx.setBlock(lo::core::GlobalPos{dim, feet.x + dx, feet.y + dy, feet.z + dz}, "minecraft:stone"),
            "wall placed"
        );
    }
    auto pos = ctx.testPos(1050);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = ctx.player().getDimensionBlockSource();
    auto* pig    = level->getSpawner().spawnMob(
        region,
        ActorDefinitionIdentifier("minecraft:pig"),
        nullptr,
        lo::game::blockPos(pos).bottomCenter(),
        false,
        false,
        false
    );
    ctx.require(pig != nullptr, "a pig spawned");
    co_await ctx.ticks(2);
    auto snbt = carry::captureActor(*pig);
    carry::removeCapturedActor(*pig);
    co_await showCarry(ctx, CarryData{.type = CarryType::Entity, .entity = snbt}, Kind::Entity, "minecraft:pig");
});

// Rejoining while carrying (e2e scenario only: the real player has the tag `lo_fp_rejoin`; without it
// both tests do nothing, so a full selftest run never leaves a carry behind). `rejoin_hold` gives the
// player a pig and keeps it; the scenario leaves the world and joins it again; `rejoin_check` waits
// until the pig is drawn in first person, holds it for a screenshot and clears it.
constexpr char const* kRejoinTag = "lo_fp_rejoin";

LO_REGISTER_SELFTEST("firstperson.rejoin_hold", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto* p = localPlayer();
    if (!p || !p->hasTag(kRejoinTag)) {
        ctx.info("not in the rejoin scenario: skipped");
        co_return;
    }
    ctx.require(!carry::isCarrying(*p), "the local player carries nothing");
    auto pos = ctx.testPos(1040);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = ctx.player().getDimensionBlockSource();
    auto* pig    = level->getSpawner().spawnMob(
        region,
        ActorDefinitionIdentifier("minecraft:pig"),
        nullptr,
        lo::game::blockPos(pos).bottomCenter(),
        false,
        false,
        false
    );
    ctx.require(pig != nullptr, "a pig spawned");
    co_await ctx.ticks(2);
    auto snbt = carry::captureActor(*pig);
    carry::removeCapturedActor(*pig);
    auto uid = p->getOrCreateUniqueID();
    carry::set(*p, CarryData{.type = CarryType::Entity, .entity = snbt});
    bool drawn = false;
    for (int i = 0; i < 200 && !drawn; ++i) {
        co_await ctx.ticks(1);
        auto st = fp::status();
        drawn   = st.kind == Kind::Entity && st.frames > 2 && st.actorAlive;
    }
    ctx.check(drawn, "drawn before leaving");
    auto* again = level->getPlayer(uid);
    ctx.check(again && carry::isCarrying(*again), "still carrying (kept for the rejoin)");
});

LO_REGISTER_SELFTEST("firstperson.rejoin_check", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto* p = localPlayer();
    if (!p || !p->hasTag(kRejoinTag)) {
        ctx.info("not in the rejoin scenario: skipped");
        co_return;
    }
    auto uid = p->getOrCreateUniqueID();
    ctx.require(carry::isCarrying(*p), "the carry survived leaving the world");
    bool drawn = false;
    for (int i = 0; i < 200 && !drawn; ++i) {
        co_await ctx.ticks(1);
        auto st = fp::status();
        drawn   = st.kind == Kind::Entity && st.what == "minecraft:pig" && st.frames > 2 && st.actorAlive;
    }
    ctx.check(drawn, "the pig is drawn in first person after rejoining");
    ctx.info("holding minecraft:pig");
    co_await ctx.ticks(kHoldTicks);
    auto level = ll::service::getLevel();
    if (auto* again = level ? level->getPlayer(uid) : nullptr) {
        carry::clear(*again);
        again->removeTag(kRejoinTag);
    }
    bool released = false;
    for (int i = 0; i < 100 && !released; ++i) {
        co_await ctx.ticks(1);
        auto now = fp::status();
        released = now.kind == Kind::None && !now.actorAlive;
    }
    ctx.check(released, "the renderer released the carry");
});

// A carried entity the client never saw (rejoined while carrying, or picked up out of its sight): a
// sheep spawned 400 blocks away, outside the client's view, with an orange fleece.
LO_REGISTER_SELFTEST("firstperson.unseen_sheep", [](Context& ctx) -> ll::coro::CoroTask<> {
    if (!localPlayer()) {
        ctx.info("no local player: skipped");
        co_return;
    }
    auto pos = ctx.testPos(1030, 400);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = ctx.player().getDimensionBlockSource();
    auto* sheep  = level->getSpawner().spawnMob(
        region,
        ActorDefinitionIdentifier("minecraft:sheep"),
        nullptr,
        lo::game::blockPos(pos).bottomCenter(),
        false,
        false,
        false
    );
    ctx.require(sheep != nullptr, "a sheep spawned");
    co_await ctx.ticks(2);
    auto tag = carry::parseNbt(carry::captureActor(*sheep));
    carry::removeCapturedActor(*sheep);
    ctx.require(tag.has_value(), "sheep NBT");
    (*tag)["Color"] = static_cast<schar>(1); // orange
    co_await showCarry(
        ctx,
        CarryData{.type = CarryType::Entity, .entity = carry::toSnbt(*tag)},
        Kind::Entity,
        "minecraft:sheep"
    );
});

#endif // LO_CLIENT
