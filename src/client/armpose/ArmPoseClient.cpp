// U13 armpose, modded clients (docs/SPEC.md §4.5): the server plays the arm animations on every
// client (`AnimateEntityPacket`, see feature/armpose/ArmPose.h); this part makes sure the client knows
// them.
//
// LeviLamina puts the mod's resource pack on the client's resource stack (its files load through the
// ResourcePackManager), but the client's actor animation group is filled before that and never sees
// the pack's animation file. A local world (modded client + integrated server) would then play
// unknown animations, i.e. nothing. So the client loads `animations/liftoff_carry.animation.json` into
// its animation group itself, once per level. On a BDS the pack also arrives as a world pack; loading
// it again only replaces the same animations.

#ifdef LO_CLIENT

#include <string>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/resources/MinEngineVersion.h"
#include "mc/resources/PackStats.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/world/actor/animation/ActorAnimationGroup.h"
#include "mc/world/actor/animation/ActorAnimationInfo.h"
#include "mc/world/actor/animation/ActorSkeletalAnimation.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "core/ArmPose.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::client::armpose {

namespace ap = core::armpose;

namespace {
constexpr char kAnimationFile[] = "animations/liftoff_carry.animation.json";

/// getActorAnimationInfo creates an empty entry for unknown names: check the parsed animation.
bool parsed(ActorAnimationGroup& group, char const* name) {
    auto info = group.getActorAnimationInfo(HashedString(name));
    return info && info->mPtr.get() != nullptr;
}

void const*& doneLevel() {
    static void const* done = nullptr;
    return done;
}

void ensureAnimations(Level& level) {
    auto& done = doneLevel();
    if (done == &level) return;
    done = &level;
    auto* ci = currentClient();
    if (!ci) return;
    auto groupPtr = ci->getActorAnimationGroup(); // the client's group (a client level has none)
    if (!groupPtr) return;
    auto& group  = *groupPtr;
    bool  before = parsed(group, ap::kNoneAnimation);
    bool  after  = before;
    if (!before) {
        std::string text;
        if (ci->getResourcePackManager().load(ResourceLocation(Core::PathView(kAnimationFile)), text)) {
            PackStats stats{};
            group.loadActorAnimation(
                text,
                Core::Path(kAnimationFile),
                stats,
                MinEngineVersion::fromString("1.21.40"),
                CurrentCmdVersion::Latest,
                false,
                ""
            );
            after = parsed(group, ap::kNoneAnimation);
        }
    }
    if (after) log().info("armpose: arm animations ready on the client{}", before ? "" : " (loaded by LiftOff)");
    else log().warn("armpose: the arm animations are missing on the client ({})", kAnimationFile);
}
} // namespace

class ArmPoseClientModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent& ev) {
            ensureAnimations(ev.level());
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { doneLevel() = nullptr; }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::client::armpose

LO_REGISTER_MODULE(lo::client::armpose::ArmPoseClientModule);

#endif
