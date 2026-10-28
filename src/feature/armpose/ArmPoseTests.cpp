// Selftests of U13 armpose (dx 1300..1399): pose values, the packets each kind of viewer gets, the
// stand-ins, and that the resource pack defines every animation the server plays.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/Packet.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"

#include "core/ArmPose.h"
#include "feature/armpose/ArmPose.h"
#include "feature/armpose/StandIn.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "game/Blocks.h"
#include "mod/Main.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace armpose = lo::feature::armpose;
namespace ap      = lo::core::armpose;
using armpose::SentPacket;
using carry::CarryData;
using carry::CarryType;

/// Script arm values for the `armpose.script` test only.
bool gScriptArms = false;
LO_REGISTER_VISUAL_PROVIDER(carry::VisualProvider{
    .name     = "armpose-test",
    .priority = -1000,
    .apply =
        [](CarryData const&, carry::CarryVisual& v) {
            if (!gScriptArms) return;
            v.rightArm[0]    = -2.0f;
            v.rightArm[1]    = 0.5f;
            v.renderLeftArm  = false;
        },
});

CarryData blockCarry(std::string_view id) {
    CarryData d;
    d.type  = CarryType::Block;
    d.block = carry::toSnbt(*lo::game::blockWith(id)->mSerializationId);
    return d;
}

std::vector<SentPacket> ofKind(std::vector<SentPacket> const& v, std::string_view kind) {
    std::vector<SentPacket> out;
    std::copy_if(v.begin(), v.end(), std::back_inserter(out), [&](auto const& p) { return p.kind == kind; });
    return out;
}

bool sameAnimations(std::vector<SentPacket> const& sent, std::vector<ap::SlotAnimation> const& want) {
    auto anims = ofKind(sent, "animate");
    if (anims.size() != want.size()) return false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (anims[i].name != want[i].animation || anims[i].detail.value("controller", "") != want[i].controller) return false;
    }
    return true;
}

void standAt(Player& p, lo::core::GlobalPos const& pos) {
    BlockPos b = lo::game::blockPos(pos);
    p.teleport(Vec3{b.x + 0.5f, static_cast<float>(b.y), b.z + 0.5f}, p.getDimensionId());
}

bool near(float a, float b) { return std::abs(a - b) < 1e-3f; }

/// A carrier and a vanilla test viewer next to each other at `dx`.
ll::coro::CoroTask<bool> setup(Context& ctx, int dx, Player*& carrierOut, Player*& viewerOut) {
    auto pos = ctx.testPos(dx);
    if (!co_await ctx.ensureLoaded(pos)) co_return false;
    auto& carrier = ctx.player();
    auto& viewer  = ctx.spawnPlayer("lo_armpose_viewer");
    standAt(carrier, pos);
    standAt(viewer, ctx.testPos(dx, 3));
    armpose::setTestViewer(viewer, true);
    armpose::setTestViewer(carrier, true);
    co_await ctx.ticks(2);
    (void)armpose::takeTestPackets(viewer);
    (void)armpose::takeTestPackets(carrier);
    carrierOut = &carrier;
    viewerOut  = &viewer;
    co_return true;
}

void teardown(Player& carrier, Player& viewer) {
    carry::clear(carrier);
    armpose::setTestViewer(viewer, false);
    armpose::setTestViewer(carrier, false);
}
} // namespace

LO_REGISTER_SELFTEST("armpose.pose", [](Context& ctx) {
    auto stone = armpose::poseFor(blockCarry("minecraft:stone"));
    ctx.require(stone.right.active && stone.left.active, "both arms are posed while carrying");
    ctx.check(near(stone.right.rotation[0], -1.0f) && stone.right.crouchX, "block: x = -1.0, crouch term applies");
    ctx.check(near(stone.right.rotation[1], 0.0f) && near(stone.left.rotation[1], 0.0f), "full block: no offset");
    ctx.check(near(stone.right.rotation[2], -0.05f) && near(stone.left.rotation[2], 0.05f), "z spread -0.05 / +0.05");

    // Offsets come from the Bedrock shape: a chest is 0.95 wide (Java 0.875), a flower pot 0.375.
    auto chest = armpose::poseFor(blockCarry("minecraft:chest"));
    float off  = (0.95f - 1.0f) / 1.5f;
    ctx.check(near(chest.right.rotation[1], off) && near(chest.left.rotation[1], -off), "chest: offset from its 0.95 shape");
    auto pot = armpose::poseFor(blockCarry("minecraft:flower_pot"));
    ctx.check(near(pot.right.rotation[1], (0.375f - 1.0f) / 1.5f), "flower pot: offset from its 0.375 shape");

    CarryData player;
    player.type   = CarryType::Player;
    player.player = "00000000-0000-0000-0000-000000000000";
    auto p        = armpose::poseFor(player);
    ctx.check(near(p.right.rotation[0], -1.3f), "player carried: x = -1.3");
    ctx.check(!armpose::poseFor(CarryData{}).right.active, "no pose without a carry");
    ctx.check(!armpose::poseFor(blockCarry("minecraft:stone"), false).right.active, "renderArms false: no pose");

    auto anims = ap::vanillaAnimations(stone);
    ctx.check(anims[0].animation == "animation.liftoff.carry.right.xc.m20", "vanilla right x animation");
    ctx.check(anims[6].animation == ap::kFirstPersonAnimation, "first-person animation while carrying");
});

LO_REGISTER_SELFTEST("armpose.script", [](Context& ctx) {
    gScriptArms = true;
    auto pose   = armpose::poseFor(blockCarry("minecraft:stone"));
    gScriptArms = false;
    ctx.check(near(pose.right.rotation[0], -2.0f) && !pose.right.crouchX, "script x replaces the computed x");
    ctx.check(near(pose.right.rotation[1], 0.5f), "script y replaces the offset");
    ctx.check(near(pose.right.rotation[2], -0.05f), "unset script axes stay computed");
    ctx.check(!pose.left.active, "render_left_arm false keeps the vanilla left arm");
    auto anims = ap::vanillaAnimations(pose);
    ctx.check(anims[0].animation == "animation.liftoff.carry.right.x.m40", "quantised script x without crouch term");
    ctx.check(anims[3].animation == ap::kNoneAnimation, "left arm slots end");
});

LO_REGISTER_SELFTEST("armpose.vanilla_viewer", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1300, carrier, viewer), "test area loaded");
    auto data = blockCarry("minecraft:chest");
    carry::set(*carrier, data);
    co_await ctx.ticks(2);
    auto want = ap::vanillaAnimations(armpose::poseFor(data));

    auto sent = armpose::takeTestPackets(*viewer);
    ctx.check(sameAnimations(sent, want), "the viewer gets the seven arm animations");
    auto adds = ofKind(sent, "add");
    ctx.require(adds.size() == 1, "the viewer gets one stand-in");
    ctx.check(adds[0].name == "minecraft:armor_stand", "a block stand-in is an armor stand");
    auto equips = ofKind(sent, "equip");
    ctx.check(equips.size() == 1 && equips[0].name == "minecraft:chest", "it wears the chest item");
    std::uint64_t id = adds[0].detail.value("id", std::uint64_t{0});
    ctx.check(id != 0 && armpose::standInShownTo(*carrier, *viewer) == id, "the stand-in id is tracked");
    ctx.check(carry::isCarrying(*carrier) && carrier->getSelectedItem().isNull(), "the carrier's hand stays empty");

    auto self = armpose::takeTestPackets(*carrier);
    ctx.check(sameAnimations(self, want), "a vanilla carrier gets its own arm pose");
    ctx.check(ofKind(self, "add").empty(), "a carrier never sees its own stand-in");

    standAt(*carrier, ctx.testPos(1302));
    co_await ctx.ticks(2);
    sent = armpose::takeTestPackets(*viewer);
    ctx.check(!ofKind(sent, "move").empty(), "the stand-in follows the carrier");
    ctx.check(ofKind(sent, "animate").empty(), "no animation re-sent while nothing changed");

    carry::clear(*carrier);
    co_await ctx.ticks(2);
    sent        = armpose::takeTestPackets(*viewer);
    auto none   = ofKind(sent, "animate");
    bool allEnd = !none.empty() && std::all_of(none.begin(), none.end(), [](auto const& p) { return p.name == ap::kNoneAnimation; });
    ctx.check(allEnd && none.size() == ap::allControllers().size(), "every slot ends when the carry ends");
    auto removes = ofKind(sent, "remove");
    ctx.check(removes.size() == 1 && removes[0].detail.value("id", std::uint64_t{0}) == id, "the stand-in is removed");
    teardown(*carrier, *viewer);
});

LO_REGISTER_SELFTEST("armpose.modded_viewer", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1310, carrier, viewer), "test area loaded");
    lo::net::setTestModded(*viewer, true);
    (void)lo::net::takeTestMessages(*viewer);
    carry::set(*carrier, blockCarry("minecraft:stone"));
    co_await ctx.ticks(3);
    ctx.check(armpose::takeTestPackets(*viewer).empty(), "a modded viewer gets no vanilla arm pose or stand-in");
    auto msgs = lo::net::takeTestMessages(*viewer);
    ctx.check(
        std::any_of(msgs.begin(), msgs.end(), [](auto const& m) { return m.first.find("CarrySyncPacket") != std::string::npos; }),
        "it gets the carry sync and draws natively"
    );
    ctx.check(armpose::standInShownTo(*carrier, *viewer) == 0, "no stand-in tracked for a modded viewer");
    lo::net::setTestModded(*viewer, false);
    co_await ctx.ticks(2);
    auto sent = armpose::takeTestPackets(*viewer);
    ctx.check(ofKind(sent, "add").size() == 1, "the same player as a vanilla client gets the stand-in");
    teardown(*carrier, *viewer);
});

LO_REGISTER_SELFTEST("armpose.entity", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1320, carrier, viewer), "test area loaded");
    auto  level  = ll::service::getLevel();
    auto& region = carrier->getDimensionBlockSource();
    Vec3  at     = lo::game::blockPos(ctx.testPos(1325)).bottomCenter();
    auto* pig    = level->getSpawner().spawnMob(region, ActorDefinitionIdentifier("minecraft:pig"), nullptr, at, false, false, false);
    ctx.require(pig != nullptr, "a pig spawned");
    pig->setNameTag("lo_armpose_pig");
    co_await ctx.ticks(1);
    CarryData d;
    d.type   = CarryType::Entity;
    d.entity = carry::captureActor(*pig);
    carry::removeCapturedActor(*pig);

    auto box = armpose::entityBox(d.entity);
    ctx.check(box.ok && near(box.width, 0.9f) && near(box.height, 0.9f), "pig box from its NBT (0.9 x 0.9)");
    auto pose = armpose::poseFor(d);
    ctx.check(near(pose.right.rotation[0], -1.3f), "entity: x = -1.3");
    ctx.check(near(pose.right.rotation[1], (0.81f - 1.0f) / 1.5f), "entity: offset from width * 0.9");

    carry::set(*carrier, d);
    co_await ctx.ticks(2);
    auto sent = armpose::takeTestPackets(*viewer);
    ctx.check(sameAnimations(sent, ap::vanillaAnimations(pose)), "entity arm animations");
    ctx.check(ofKind(sent, "animate")[0].name == "animation.liftoff.carry.right.xc.m26", "x = -1.3 rad");
    auto adds = ofKind(sent, "add");
    ctx.require(adds.size() == 1, "one stand-in");
    ctx.check(adds[0].name == "minecraft:pig", "the stand-in is a pig");
    float want = 0.6f * 1.666f * (10.0f - 0.81f) * 0.08f;
    ctx.check(near(adds[0].detail.value("scale", 0.0f), want), "scaled like Carry On's third-person view");
    ctx.check(ofKind(sent, "equip").empty(), "an entity stand-in wears nothing");
    teardown(*carrier, *viewer);
});

LO_REGISTER_SELFTEST("armpose.player", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1330, carrier, viewer), "test area loaded");
    CarryData d;
    d.type   = CarryType::Player;
    d.player = viewer->getUuid().asString();
    carry::set(*carrier, d);
    co_await ctx.ticks(2);
    auto sent = armpose::takeTestPackets(*viewer);
    ctx.check(!ofKind(sent, "animate").empty() && ofKind(sent, "animate")[0].name == "animation.liftoff.carry.right.xc.m26", "player carried: x = -1.3");
    ctx.check(ofKind(sent, "add").empty(), "a carried player needs no stand-in");
    teardown(*carrier, *viewer);
});

LO_REGISTER_SELFTEST("armpose.view_range", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1340, carrier, viewer), "test area loaded");
    carry::set(*carrier, blockCarry("minecraft:stone"));
    co_await ctx.ticks(2);
    auto sent = armpose::takeTestPackets(*viewer);
    std::uint64_t id = ofKind(sent, "add").empty() ? 0 : ofKind(sent, "add")[0].detail.value("id", std::uint64_t{0});
    ctx.require(id != 0, "stand-in shown nearby");

    // Far beyond any view distance: the client dropped the carrier, the stand-in goes too.
    auto far = carrier->getPosition();
    carrier->teleport(Vec3{far.x + 2000.0f, far.y, far.z}, carrier->getDimensionId());
    co_await ctx.ticks(2);
    sent         = armpose::takeTestPackets(*viewer);
    auto removes = ofKind(sent, "remove");
    ctx.check(removes.size() == 1 && removes[0].detail.value("id", std::uint64_t{0}) == id, "out of view: stand-in removed");
    ctx.check(armpose::standInShownTo(*carrier, *viewer) == 0, "nothing shown out of view");

    standAt(*carrier, ctx.testPos(1340));
    co_await ctx.ticks(2);
    sent = armpose::takeTestPackets(*viewer);
    ctx.check(ofKind(sent, "add").size() == 1 && ofKind(sent, "animate").size() == ap::allControllers().size(), "back in view: pose and stand-in again");
    teardown(*carrier, *viewer);
});

LO_REGISTER_SELFTEST("armpose.reannounce", [](Context& ctx) -> ll::coro::CoroTask<> {
    Player *carrier = nullptr, *viewer = nullptr;
    ctx.require(co_await setup(ctx, 1350, carrier, viewer), "test area loaded");
    carry::set(*carrier, blockCarry("minecraft:stone"));
    co_await ctx.ticks(2);
    (void)armpose::takeTestPackets(*viewer);
    // A client (re)learns about the carrier (AddPlayerPacket): the animations are played again.
    auto pkt = carrier->tryCreateAddActorPacket();
    ctx.check(pkt != nullptr, "an AddPlayerPacket is built");
    co_await ctx.ticks(5);
    auto sent = armpose::takeTestPackets(*viewer);
    ctx.check(ofKind(sent, "animate").size() == ap::allControllers().size(), "arm pose re-sent after the carrier was re-announced");
    ctx.check(ofKind(sent, "add").empty(), "the stand-in is not duplicated");
    teardown(*carrier, *viewer);
});

// For the client e2e scenario (screenshots): only runs when a real player has the tag
// `lo_armpose_showcase`, so `/carryon selftest` without a glob skips it at once. Puts three simulated
// carriers (chest, pig, sneaking with a barrel) in front of the tagged player, gives that player a
// chest too, holds everything for `kShowcaseTicks` and clears it.
LO_REGISTER_SELFTEST("armpose.showcase", [](Context& ctx) -> ll::coro::CoroTask<> {
    constexpr int kShowcaseTicks = 900;
    Player*       real           = nullptr;
    ll::service::getLevel()->forEachPlayer([&](Player& p) {
        if (!p.isSimulatedPlayer() && p.hasTag("lo_armpose_showcase")) real = &p;
        return real == nullptr;
    });
    if (!real) {
        ctx.info("skipped: no player tagged lo_armpose_showcase");
        co_return;
    }
    auto realId = real->getOrCreateUniqueID();
    auto pos    = ctx.testPos(1390);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos base = lo::game::blockPos(pos);
    real->teleport(Vec3{base.x + 0.5f, static_cast<float>(base.y), base.z + 0.5f}, real->getDimensionId(), Vec2{10.0f, 0.0f});

    auto& level  = *ll::service::getLevel();
    auto& region = real->getDimensionBlockSource();
    Vec3  pigAt  = Vec3{base.x + 0.5f, static_cast<float>(base.y), base.z + 8.5f};
    auto* pig    = level.getSpawner().spawnMob(region, ActorDefinitionIdentifier("minecraft:pig"), nullptr, pigAt, false, false, false);
    ctx.require(pig != nullptr, "a pig spawned");
    CarryData pigCarry;
    pigCarry.type   = CarryType::Entity;
    pigCarry.entity = carry::captureActor(*pig);
    carry::removeCapturedActor(*pig);

    struct Show {
        float      dx;
        CarryData  data;
        bool       sneak;
    };
    std::vector<Show> shows{{-2.0f, blockCarry("minecraft:chest"), false}, {0.0f, pigCarry, false}, {2.0f, blockCarry("minecraft:barrel"), true}};
    std::vector<SimulatedPlayer*> carriers;
    for (std::size_t i = 0; i < shows.size(); ++i) {
        auto& sp = ctx.spawnPlayer("lo_armpose_carrier" + std::to_string(i));
        sp.teleport(Vec3{base.x + 0.5f + shows[i].dx, static_cast<float>(base.y), base.z + 4.5f}, sp.getDimensionId(), Vec2{0.0f, 180.0f});
        carriers.push_back(&sp);
    }
    co_await ctx.ticks(5);
    for (std::size_t i = 0; i < shows.size(); ++i) {
        carriers[i]->simulateSetBodyRotation(180.0f);
        if (shows[i].sneak) carriers[i]->simulateSneaking();
        carry::set(*carriers[i], shows[i].data);
    }
    carry::set(*real, blockCarry("minecraft:chest"));
    ctx.info("showcase ready");
    for (int t = 0; t < kShowcaseTicks; t += 20) {
        co_await ctx.ticks(20);
        for (auto* sp : carriers) sp->simulateSetBodyRotation(180.0f);
    }
    for (auto* sp : carriers) carry::clear(*sp);
    if (auto* again = level.getPlayer(realId)) {
        carry::clear(*again);
        again->removeTag("lo_armpose_showcase");
    }
});

LO_REGISTER_SELFTEST("armpose.pack", [](Context& ctx) {
    auto file = lo::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "LiftOff_RP" / "animations"
              / "liftoff_carry.animation.json";
    std::ifstream in(file);
    ctx.require(in.good(), "the resource pack has animations/liftoff_carry.animation.json");
    auto j = nlohmann::json::parse(in, nullptr, false);
    ctx.require(j.is_object() && j.contains("animations"), "the animation file parses");
    auto const& anims   = j["animations"];
    int         missing = 0;
    for (auto side : {ap::Side::Right, ap::Side::Left}) {
        for (auto axis : {ap::Axis::X, ap::Axis::Y, ap::Axis::Z}) {
            for (int i = -ap::kMaxIndex; i <= ap::kMaxIndex; ++i) {
                if (!anims.contains(ap::animationName(side, axis, i))) ++missing;
                if (axis == ap::Axis::X && !anims.contains(ap::animationName(side, axis, i, true))) ++missing;
            }
        }
    }
    ctx.check(missing == 0, "every quantised arm animation exists (missing: " + std::to_string(missing) + ")");
    ctx.check(!anims.contains(ap::animationName(ap::Side::Right, ap::Axis::X, ap::kMaxIndex + 1)), "the table ends at kMaxIndex");
    auto sneak = "q.is_sneaking * " + std::to_string(static_cast<int>(ap::kSneakRootDegrees));
    ctx.check(anims[ap::animationName(ap::Side::Left, ap::Axis::X, 0)].dump().find(sneak) != std::string::npos, "sneak tilt matches kSneakRootDegrees");
    for (char const* name : {ap::kFirstPersonAnimation, ap::kNoneAnimation, ap::kNativeAnimation}) {
        ctx.check(anims.contains(name), std::string(name) + " exists");
    }
});
