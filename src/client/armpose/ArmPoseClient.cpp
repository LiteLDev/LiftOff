// U13 armpose, modded clients (docs/SPEC.md §4.5): every player the client sees with a synced carry
// plays `animation.liftoff.carry.native`, whose arm values come from molang variables set here with the
// exact Carry On values (no quantisation) and this client's `renderArms` / `rotateEntitiesSideways`.
// The server never sends the vanilla animations to a modded client.

#ifdef LO_CLIENT

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/molang/MolangVersion.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/animation/AnimationComponent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "core/ArmPose.h"
#include "feature/armpose/ArmPose.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::client::armpose {

namespace ap = core::armpose;
using feature::carry::CarryData;

namespace {
constexpr int kReplayInterval = 100; ///< ticks: play the native animation again (cheap; survives resets)

struct Tracked {
    CarryData     data;
    ap::Pose      pose;
    void const*   actor   = nullptr; ///< identity only, never dereferenced
    std::uint64_t runtime = 0;
    int           sinceReplay = 0;
    bool          renderArms  = true;
    bool          sideways    = false;
};
std::unordered_map<std::int64_t, Tracked>& tracked() {
    static std::unordered_map<std::int64_t, Tracked> t;
    return t;
}

/// Same carried thing (the key state is part of the record but does not change the pose).
bool sameCarry(CarryData a, CarryData const& b) {
    a.keyPressed = b.keyPressed;
    return a == b;
}

void setVar(Actor& actor, std::string const& name, float value) {
    actor.mMolangVariables->setMolangVariable(HashedString(name), MolangScriptArg(value));
}

void applyPose(Actor& actor, ap::Pose const& pose) {
    for (auto side : {ap::Side::Right, ap::Side::Left}) {
        auto const& arm = pose.arm(side);
        setVar(actor, ap::nativeVariable(side, ""), arm.active ? 1.0f : 0.0f);
        setVar(actor, ap::nativeVariable(side, "x"), ap::toDegrees(arm.rotation[0]));
        setVar(actor, ap::nativeVariable(side, "y"), ap::toDegrees(arm.rotation[1]));
        setVar(actor, ap::nativeVariable(side, "z"), ap::toDegrees(arm.rotation[2]));
        setVar(actor, ap::nativeVariable(side, "xc"), arm.crouchX ? 1.0f : 0.0f);
    }
}

void play(Actor& actor) {
    actor.getAnimationComponent().editGlobalAnimationData(
        HashedString(ap::kNativeAnimation),
        0.0f,
        "0",
        MolangVersion::Latest,
        "",
        ap::kNativeController
    );
}

void tick(Level& level) {
    auto const& cfg   = config::get<feature::carry::CarryClientConfig>().settings;
    auto&       known = tracked();
    std::unordered_map<std::int64_t, bool> seen;
    level.forEachPlayer([&](Player& p) {
        auto id   = p.getOrCreateUniqueID().rawID;
        auto data = client::carry::carriedOf(id);
        auto it   = known.find(id);
        if (!data) {
            if (it != known.end()) {
                applyPose(p, ap::Pose{}); // arms back to vanilla; the animation adds nothing now
                known.erase(it);
            }
            return true;
        }
        seen[id] = true;
        bool fresh = it == known.end();
        auto& t    = known[id];
        bool changed = fresh || !sameCarry(t.data, *data) || t.renderArms != cfg.renderArms || t.sideways != cfg.rotateEntitiesSideways;
        if (changed) {
            t.data       = *data;
            t.renderArms = cfg.renderArms;
            t.sideways   = cfg.rotateEntitiesSideways;
            t.pose       = feature::armpose::poseFor(*data, cfg.renderArms, cfg.rotateEntitiesSideways, &level);
        }
        bool newActor = t.actor != &p || t.runtime != p.getRuntimeID().rawID;
        if (changed || newActor) applyPose(p, t.pose);
        if (newActor || ++t.sinceReplay >= kReplayInterval) {
            play(p);
            t.actor       = &p;
            t.runtime     = p.getRuntimeID().rawID;
            t.sinceReplay = 0;
        }
        if (changed) {
            log().info(
                "armpose: native pose for {} (right {:.3f} {:.3f} {:.3f}, molang {:.2f})",
                p.getRealName(),
                t.pose.right.rotation[0],
                t.pose.right.rotation[1],
                t.pose.right.rotation[2],
                p.evalMolang(ap::nativeVariable(ap::Side::Right, "x"))
            );
        }
        return true;
    });
    std::erase_if(known, [&](auto const& kv) { return !seen.contains(kv.first); });
}
} // namespace

class ArmPoseClientModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent& ev) {
            tick(ev.level());
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { tracked().clear(); }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        tracked().clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::client::armpose

LO_REGISTER_MODULE(lo::client::armpose::ArmPoseClientModule);

#endif
