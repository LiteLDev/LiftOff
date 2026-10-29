// Selftests of U11 (third-person rendering, dx 1100..1199).
//
// The scene tests run on the (integrated or dedicated) server: they put the first real player (the
// viewer) in front of simulated carriers that carry a chest + a furnace / a pig + a cow, standing or
// sneaking, and make the viewer carry a chest itself. Each scene logs `SELFTEST INFO <test>: ready
// <scene>` and holds it for a few seconds, so the client e2e scenario (tools/e2e/client/scenarios/
// thirdperson.ps1) can take screenshots, also with a dedicated server (`-Mode remote-mod`), where the
// carries reach the client only through CarrySync.
//
// This file is built on both platforms (the BDS runs the scenes for a remote modded client); the client
// build also checks what the renderer drew (ThirdPerson.h), so the scenes double as assertions in local
// worlds. Without a real player (the server harness) the scenes have nobody to show and pass at once.

#include <cmath>
#include <string>
#include <vector>

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

#ifdef LO_CLIENT
#include "client/thirdperson/ThirdPerson.h"
#endif

namespace {

using lo::selftest::Context;
namespace carry = lo::feature::carry;
using carry::CarryData;
using carry::CarryType;

/// Ticks a scene is held for the screenshots.
constexpr int kHold = 60;

/// The first real (not simulated) player: the one who watches the scene.
Player* viewerOf() {
    auto    level  = ll::service::getLevel();
    Player* viewer = nullptr;
    if (!level) return nullptr;
    level->forEachPlayer([&](Player& p) {
        if (p.isSimulated()) return true;
        viewer = &p;
        return false;
    });
    return viewer;
}

std::int64_t idOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

CarryData blockCarry(std::string const& id) {
    CarryData d;
    d.type = CarryType::Block;
    if (auto* b = lo::game::blockWith(id)) d.block = carry::toSnbt(*b->mSerializationId);
    return d;
}

/// The scene layout: the viewer at `base` looking south (+Z) at the carriers 4 blocks away, who face it.
struct Stage {
    Player*              viewer = nullptr;
    lo::core::GlobalPos  base;
    std::vector<Player*> carriers;

    [[nodiscard]] Vec3 at(float dx, float dz) const {
        return Vec3{static_cast<float>(base.x) + 0.5f + dx, static_cast<float>(base.y), static_cast<float>(base.z) + 0.5f + dz};
    }
};

/// Puts the viewer and `count` simulated carriers in place. nullopt: nobody to show the scene to.
ll::coro::CoroTask<std::optional<Stage>> setUp(Context& ctx, int count) {
    Stage s;
    s.viewer = viewerOf();
    if (!s.viewer) {
        ctx.info("no real player to watch the scene; skipped");
        co_return std::nullopt;
    }
    s.base = ctx.testPos(1100);
    ctx.require(co_await ctx.ensureLoaded(s.base), "test area loaded");
    auto dim = DimensionType(s.base.dim);
    s.viewer->teleport(s.at(0.f, 0.f), dim, Vec2{12.f, 0.f});
    // The test player loaded the area at `base`; out of the way unless it is a carrier.
    if (count == 0) ctx.player().teleport(s.at(12.f, -12.f), dim);
    for (int i = 0; i < count; ++i) {
        Player& c = i == 0 ? ctx.player() : static_cast<Player&>(ctx.spawnPlayer());
        float   dx = count == 1 ? 0.f : (i == 0 ? -1.1f : 1.1f);
        c.teleport(s.at(dx, 4.f), dim, Vec2{0.f, 180.f});
        if (c.isSimulated()) static_cast<SimulatedPlayer&>(c).simulateSetBodyRotation(180.f);
        s.carriers.push_back(&c);
    }
    co_await ctx.ticks(10);
    co_return s;
}

void tearDown(Stage const& s) {
    for (auto* c : s.carriers) {
        carry::clear(*c);
        c->setSneaking(false);
    }
}

#ifdef LO_CLIENT
/// The client drew `carrier`'s carry as `kind`, in front of the carrier (towards the viewer), at
/// chest height (`low`: sneaking).
void checkDrawn(Context& ctx, Player& carrier, carry::CarryVisual::Kind kind, std::string const& what) {
    auto st = lo::client::thirdperson::statsOf(idOf(carrier));
    ctx.check(st.frames > 0, what + ": drawn");
    if (st.frames == 0) return;
    ctx.check(st.kind == static_cast<int>(kind), what + ": drawn as the expected kind");
    ctx.check(st.displayActor == (kind == carry::CarryVisual::Kind::Entity), what + ": display actor iff entity");
    Vec3  p    = carrier.getPosition();
    float base = carrier.getAABB().min.y;
    ctx.info(
        what + ": centre " + std::to_string(st.center[0] - p.x) + " " + std::to_string(st.center[1] - base) + " "
        + std::to_string(st.center[2] - p.z) + " from the carrier's feet"
    );
    ctx.check(std::abs(st.center[0] - p.x) < 0.6f, what + ": centred on the carrier (x)");
    ctx.check(st.center[2] < p.z - 0.15f && st.center[2] > p.z - 1.2f, what + ": in front of the carrier (towards the viewer)");
    ctx.check(st.center[1] > base + 0.3f && st.center[1] < base + 1.6f, what + ": at chest height");
}
#endif

ll::coro::CoroTask<> showBlocks(Context& ctx, bool sneaking) {
    auto stage = co_await setUp(ctx, 2);
    if (!stage) co_return;
    auto& s = *stage;
    carry::set(*s.carriers[0], blockCarry("minecraft:chest"));
    carry::set(*s.carriers[1], blockCarry("minecraft:furnace"));
    for (auto* c : s.carriers) c->setSneaking(sneaking);
#ifdef LO_CLIENT
    lo::client::thirdperson::resetStats();
#endif
    co_await ctx.ticks(20);
#ifdef LO_CLIENT
    checkDrawn(ctx, *s.carriers[0], carry::CarryVisual::Kind::Block, "chest");
    checkDrawn(ctx, *s.carriers[1], carry::CarryVisual::Kind::Block, "furnace");
#endif
    ctx.info(sneaking ? "ready blocks-sneaking" : "ready blocks");
    co_await ctx.ticks(kHold);
    std::int64_t chestCarrier = idOf(*s.carriers[0]);
    tearDown(s);
    co_await ctx.ticks(10);
#ifdef LO_CLIENT
    ctx.check(!lo::client::thirdperson::hasCacheEntry(chestCarrier), "the cache entry is dropped when the carry ends");
#else
    (void)chestCarrier;
#endif
}

} // namespace

LO_REGISTER_SELFTEST("thirdperson.scene_blocks", [](Context& ctx) -> ll::coro::CoroTask<> {
    co_await showBlocks(ctx, false);
});

LO_REGISTER_SELFTEST("thirdperson.scene_blocks_sneaking", [](Context& ctx) -> ll::coro::CoroTask<> {
    co_await showBlocks(ctx, true);
});

LO_REGISTER_SELFTEST("thirdperson.scene_entities", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto stage = co_await setUp(ctx, 2);
    if (!stage) co_return;
    auto& s      = *stage;
    auto  level  = ll::service::getLevel();
    auto& region = s.carriers[0]->getDimensionBlockSource();
    char const* ids[] = {"minecraft:pig", "minecraft:cow"};
    for (int i = 0; i < 2; ++i) {
        auto* mob = level->getSpawner().spawnMob(region, ActorDefinitionIdentifier(ids[i]), nullptr, s.at(4.f + i, 8.f), false, false, false);
        ctx.require(mob != nullptr, std::string(ids[i]) + " spawned");
        co_await ctx.ticks(2);
        CarryData d;
        d.type   = CarryType::Entity;
        d.entity = carry::captureActor(*mob);
        carry::removeCapturedActor(*mob);
        carry::set(*s.carriers[i], d);
    }
#ifdef LO_CLIENT
    lo::client::thirdperson::resetStats();
#endif
    co_await ctx.ticks(20);
#ifdef LO_CLIENT
    checkDrawn(ctx, *s.carriers[0], carry::CarryVisual::Kind::Entity, "pig");
    checkDrawn(ctx, *s.carriers[1], carry::CarryVisual::Kind::Entity, "cow");
#endif
    ctx.info("ready entities");
    co_await ctx.ticks(kHold);
    for (auto* c : s.carriers) c->setSneaking(true);
    co_await ctx.ticks(10);
    ctx.info("ready entities-sneaking");
    co_await ctx.ticks(kHold);
    tearDown(s);
});

LO_REGISTER_SELFTEST("thirdperson.scene_local", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto stage = co_await setUp(ctx, 0);
    if (!stage) co_return;
    auto& s = *stage;
    s.viewer->teleport(s.at(0.f, 0.f), DimensionType(s.base.dim), Vec2{0.f, 0.f});
    carry::set(*s.viewer, blockCarry("minecraft:chest"));
#ifdef LO_CLIENT
    // The local player is drawn only in the third-person views.
    namespace tp = lo::client::thirdperson;
    int  before = tp::perspective();
    auto id     = idOf(*s.viewer);
    for (int view : {1, 2}) {
        tp::setPerspective(view);
        co_await ctx.ticks(10);
        tp::resetStats();
        co_await ctx.ticks(10);
        auto st = tp::statsOf(id);
        ctx.check(st.frames > 0, "the local carry is drawn in third-person view " + std::to_string(view));
        ctx.info(view == 1 ? "ready local-back" : "ready local-front");
        co_await ctx.ticks(kHold);
    }
    tp::setPerspective(0);
    co_await ctx.ticks(10);
    tp::resetStats();
    co_await ctx.ticks(10);
    ctx.check(tp::statsOf(id).frames == 0, "the local carry is not drawn in first person (U10 draws it)");
    tp::setPerspective(before < 0 ? 0 : before);
#else
    // A remote modded client: the e2e scenario switches the camera (F5) while the scene is held.
    ctx.info("ready local");
    co_await ctx.ticks(kHold * 4);
#endif
    carry::clear(*s.viewer);
    co_await ctx.ticks(2);
});
