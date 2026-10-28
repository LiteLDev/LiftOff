#include "feature/entitypickup/EntityPickup.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <new>
#include <string>
#include <utility>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/string/StaticOptimizedString.h"
#include "mc/entity/components_json_legacy/Interaction.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components/MobHurtTimeComponent.h"
#include "mc/entity/utilities/leashable_utility/LeashableUtility.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorSwingSource.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/gamemode/SurvivalMode.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/level/ILevel.h"
#include "mc/world/level/Level.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Feedback.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::entitypickup {

namespace carry = lo::feature::carry;

std::string_view resultName(Result result) {
    switch (result) {
    case Result::PickedUp: return "picked_up";
    case Result::Ignored: return "ignored";
    case Result::Preconditions: return "preconditions";
    case Result::Invulnerable: return "invulnerable";
    case Result::Removed: return "removed";
    case Result::Owner: return "owner";
    case Result::Lists: return "lists";
    case Result::Hostile: return "hostile";
    case Result::Size: return "size";
    case Result::Condition: return "condition";
    case Result::Protection: return "protection";
    case Result::Script: return "script";
    }
    return "unknown";
}

namespace {

/// Java `invulnerableTime != 0`: Bedrock keeps the post-damage cooldown in `mInvulnerableTime` and the
/// hurt animation timer in the mob's `MobHurtTimeComponent`; either one means "just hurt".
bool inHurtFrames(Actor& entity) {
    if (entity.mInvulnerableTime > 0) return true;
    if (auto hurt = entity.getEntityContext().tryGetComponent<MobHurtTimeComponent>()) return hurt->mValue > 0;
    return false;
}

/// Tamed with an owner that is not `player` (Carry On: `TamableAnimal` owner check). Never overridable.
bool ownedByAnother(Actor const& entity, Player const& player) {
    if (!entity.isTame()) return false;
    auto owner = entity.getOwnerId();
    if (owner == ActorUniqueID::INVALID_ID() || owner.rawID == 0) return false;
    return !(owner == player.getOrCreateUniqueID());
}

/// The server-side player (BDS, or the integrated server of the client build). LeviLamina's
/// `Actor::isClientSide()` only tests for a null level, so ask the level itself.
bool isServerPlayer(Player const& player) {
    ILevel* level = player.mLevel;
    return level && !level->isClientSide();
}

// ---- feedback packets --------------------------------------------------------------------------
// `carry::playSound` / `carry::swing` build `PlaySoundPacket` / `AnimatePacket` with LeviLamina's
// template constructor, which leaves the packet's `mSerializationMode` uninitialised: a vanilla
// client that receives such a packet disconnects (seen in the remote-vanilla client e2e). Until the
// spine is fixed, this unit sends the same packets built by the game's own factory.

template <class T>
std::shared_ptr<T> makePacket(MinecraftPacketIds id) {
    return std::static_pointer_cast<T>(MinecraftPackets::createPacket(id));
}

/// `carry::playSound(actor, ...)` with a factory-built packet.
void playSoundAt(Actor const& at, std::string_view name, float volume, float pitch) {
    auto pkt = makePacket<PlaySoundPacket>(MinecraftPacketIds::PlaySound);
    if (!pkt) return;
    Vec3 pos                       = at.getPosition();
    pkt->mName                     = std::string(name);
    pkt->mPos                      = BlockPos{
        static_cast<int>(pos.x * 8.0f),
        static_cast<int>(pos.y * 8.0f),
        static_cast<int>(pos.z * 8.0f),
    }; // 1/8 blocks
    pkt->mVolume                   = volume;
    pkt->mPitch                    = pitch;
    pkt->mLoopCount                = 0;
    pkt->mServerSoundHandle        = std::nullopt;
    pkt->mBypassListenerRangeCheck = false;
    pkt->mPlaybackPositionSeconds  = std::nullopt;
    pkt->sendTo(game::blockPosOf(pos), at.getDimensionId());
}

/// `carry::swing(player)` with a factory-built packet (seen by everyone, the player included).
void swingArm(Player& player) {
    player.swing(ActorSwingSource::Interact, HandSlot::Mainhand);
    auto pkt = makePacket<AnimatePacket>(MinecraftPacketIds::Animate);
    if (!pkt) return;
    pkt->mRuntimeId   = player.getRuntimeID();
    pkt->mAction      = AnimatePacketPayload::Action::Swing;
    pkt->mData        = 0.0f;
    pkt->mSwingSource = ActorSwingSource::Interact;
    pkt->sendTo(static_cast<Actor const&>(player));
}

} // namespace

namespace {

/// Steps 1-8 of SPEC §3.2 (no side effects). `overridden`: the script `override_checks` result.
Result basicChecks(Player& player, Actor& entity, carry::PickupTarget const& target, bool& overridden) {
    // Players (and the carrier itself) are left to the player-carry unit (U5).
    if (&entity == &player || entity.isPlayer()) return Result::Ignored;

    // 1-2. Distance (feet to feet, creative scale 5/3), mode, hands, carrying, tick guard, key.
    if (!carry::canStartPickup(player, entity.getFeetPos(), true)) return Result::Preconditions;
    // 3. Post-damage invulnerability.
    if (inHurtFrames(entity)) return Result::Invulnerable;
    // 4. Removed / dead.
    if (entity.mRemoved || !entity.isAlive()) return Result::Removed;
    // 5. Tamed by someone else (never overridable).
    if (ownedByAnother(entity, player)) return Result::Owner;

    // 6. Script override_checks.
    overridden    = carry::overrideChecks(target);
    auto const& s = config::get<carry::CarryConfig>().settings;

    // 7. Entity lists: babies pass with allowBabies.
    if (!carry::isEntityAllowed(entity) && !overridden && !(s.allowBabies && entity.isBaby())) return Result::Lists;

    // 8. Survival only: hostile mobs and size (relative to the carrier's scale; size never overridable).
    if (!carry::isCreative(player)) {
        if (entity.hasFamily(HashedString("monster")) && !s.pickupHostileMobs && !overridden) return Result::Hostile;
        double factor = s.relativePlayerScale ? 1.0 / carry::playerScale(player) : 1.0;
        double height = carry::entityHeight(entity) * factor;
        double width  = carry::entityWidth(entity) * factor;
        if (height > s.maxEntityHeight || width > s.maxEntityWidth) return Result::Size;
    }
    return Result::PickedUp;
}

carry::PickupTarget targetOf(Player& player, Actor& entity) {
    carry::PickupTarget target{player, player.getDimensionBlockSource()};
    target.entity = &entity;
    return target;
}

} // namespace

Result precheck(Player& player, Actor& entity) {
    bool overridden = false;
    return basicChecks(player, entity, targetOf(player, entity), overridden);
}

Result tryPickup(Player& player, Actor& entity) {
    auto target     = targetOf(player, entity);
    bool overridden = false;
    if (auto r = basicChecks(player, entity, target, overridden); r != Result::PickedUp) return r;

    // 9. Custom pickup conditions. 10. Protection.
    if (!carry::customConditions(target)) return Result::Condition;
    if (!carry::protectionAllows(target)) return Result::Protection;

    // 11. Script conditions; activates the script and runs its commandPickup. The `beforePickup` stage
    // combines the condition check with commandPickup, so the command runs before the eject below
    // (Carry On runs it right after); running it after the eject would leave the passengers ejected
    // and the lead dropped when a script condition refuses.
    auto const       id   = entity.getOrCreateUniqueID();
    carry::CarryData data = carry::get(player);
    data.clear();
    if (!carry::beforePickup(target, data)) return Result::Script;
    // A pickup command may have removed the entity (/kill) or recreated it in another dimension (/tp):
    // look it up again instead of trusting the reference.
    auto  level = ll::service::getLevel();
    Actor* again = level ? level->fetchEntity(id, false) : nullptr;
    if (!again || again != &entity || entity.mRemoved || !entity.isAlive()) return Result::Removed;

    // 13. Eject passengers, stop riding, drop the lead, then save and remove (not killed, no drops).
    int const amplifier = carry::entityAmplifier(entity, player);
    if (entity.isLeashed()) LeashableUtility::unleash(entity, LeashableUtility::UnleashReason::Broken);
    entity.removeAllPassengers(false, false);
    if (entity.getVehicle()) entity.stopRiding(true, false, false, false);

    std::string const type = entity.getTypeName();
    data.type              = carry::CarryType::Entity;
    data.entity            = carry::captureActor(entity);
    carry::removeCapturedActor(entity);
    carry::set(player, std::move(data));

    playSoundAt(player, carry::sound::kArmorEquip, 1.0f, 0.5f);
    swingArm(player);
    carry::applySlowness(player, amplifier);
    log().info("carry: {} picked up entity {}", player.getRealName(), type);
    return Result::PickedUp;
}

namespace {

/// Set while one of our hooks runs, so `SurvivalMode::interact` calling down into
/// `GameMode::interact` does not attempt the pickup twice.
thread_local bool gInHook = false;

std::optional<Result> gLastHookResult;

struct HookScope {
    bool previous;
    HookScope() : previous(gInHook) { gInHook = true; }
    ~HookScope() { gInHook = previous; }
};

/// True when the interaction was consumed by a pickup.
bool handleInteract(Player& player, Actor& entity) {
    if (gInHook || !isServerPlayer(player)) return false;
    auto result      = tryPickup(player, entity);
    gLastHookResult = result;
    return result == Result::PickedUp;
}

LL_TYPE_INSTANCE_HOOK(
    GameModeInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&        entity,
    Vec3 const&   location,
    HandSlot      handSlot
) {
    if (handleInteract(mPlayer, entity)) return true;
    HookScope scope;
    return origin(entity, location, handSlot);
}

LL_TYPE_INSTANCE_HOOK(
    SurvivalModeInteractHook,
    ll::memory::HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$interact,
    bool,
    Actor&         entity,
    Vec3 const&    location,
    HandSlot const handSlot
) {
    if (handleInteract(mPlayer, entity)) return true;
    HookScope scope;
    return origin(entity, location, handSlot);
}

/// `Player::interact` is what `GameMode::interact` runs for the vanilla interaction, and what
/// `SimulatedPlayer::simulateInteract` calls directly (it skips GameMode). Hooked too so every
/// interaction path picks up; the shared `gInHook` guard keeps it from running a second attempt when
/// it is reached from the GameMode hooks.
LL_TYPE_INSTANCE_HOOK(
    PlayerInteractHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::interact,
    InteractionResult,
    Actor&      entity,
    Vec3 const& location
) {
    if (handleInteract(*this, entity)) {
        InteractionResult done{};
        done.mSuccess = true;
        done.mSwing   = false; // swingArm() already swung
        return done;
    }
    HookScope scope;
    return origin(entity, location);
}

/// The interaction offered for a possible pickup: text = RP key (touch "interact" button), the callback
/// does nothing (the `GameMode::interact` hook picks up before vanilla would run it).
Interaction carryInteraction() {
    static constexpr char kText[] = "action.interact.liftoff_carry";
    // StaticOptimizedString has no callable default constructor: a zeroed one is empty, then `_set`
    // points it at the static text (Static storage owns nothing, so no destructor is needed).
    alignas(Bedrock::StaticOptimizedString) std::byte raw[sizeof(Bedrock::StaticOptimizedString)]{};
    auto* text = std::launder(reinterpret_cast<Bedrock::StaticOptimizedString*>(raw));
    text->_set(kText, sizeof(kText) - 1, Bedrock::StaticOptimizedString::StorageType::Static);
    return Interaction::Success(*text, [] {});
}

/// [LO] Touch controls: a mouse/keyboard client sends the interact transaction for any entity under the
/// crosshair (verified with a vanilla client on BDS), but touch clients only get an "interact" button
/// when the server reports interact text for the entity (`Player::getInteractText`, synced from the
/// server's `Actor::getInteraction`). So while the §3.2 checks 1-8 pass (sneaking, empty hands, in
/// reach, allowed entity, ...), an entity without a vanilla interaction offers a "Carry" interaction
/// (RP text `action.interact.liftoff_carry`). An entity with a vanilla interaction keeps it: the
/// interact hooks pick up before the vanilla interaction runs.
LL_TYPE_INSTANCE_HOOK(
    GetInteractionHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$getInteraction,
    Interaction,
    Player&     player,
    Vec3 const& location
) {
    Interaction result = origin(player, location);
    if (static_cast<bool>(*result.mCallback) || gInHook || !isServerPlayer(player)) return result;
    if (precheck(player, *this) != Result::PickedUp) return result;
    return carryInteraction();
}

} // namespace

std::optional<Result> takeLastHookResult() { return std::exchange(gLastHookResult, std::nullopt); }

struct EntityPickupModule {
    void install() {
        GameModeInteractHook::hook();
        SurvivalModeInteractHook::hook();
        GetInteractionHook::hook();
        PlayerInteractHook::hook();
    }
    void uninstall() {
        PlayerInteractHook::unhook();
        GetInteractionHook::unhook();
        SurvivalModeInteractHook::unhook();
        GameModeInteractHook::unhook();
    }
};

} // namespace lo::feature::entitypickup

LO_REGISTER_MODULE(lo::feature::entitypickup::EntityPickupModule);
