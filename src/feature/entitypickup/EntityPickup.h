#pragma once

// Entity pickup (U3, docs/SPEC.md §3.2): sneak (or the Carry key) + right-click a mob with both hands
// empty to pick it up. Server thread.
//
// Trigger: `GameMode::$interact` (and `SurvivalMode::$interact`) on the server side. When the pickup
// succeeds the vanilla interaction (milking, trading, saddling, ...) does not run; when it is refused
// the vanilla interaction runs unchanged.
//
// Players are NOT handled here: the hook returns without touching an interaction with a player
// (`Actor::isPlayer()`), so the player-carry unit (U5) can handle it in its own hook. U5 runs the same
// §3.2 checks (steps 1-11) for players.

#include <optional>
#include <string_view>

class Actor;
class Player;

namespace lo::feature::entitypickup {

/// The outcome of a pickup attempt, in the order of the checks (SPEC §3.2).
enum class Result {
    PickedUp,      ///< the entity is now carried
    Ignored,       ///< not ours: the target is a player (U5) or the carrier itself
    Preconditions, ///< `carry::canStartPickup` failed (distance, mode, hands, carrying, tick guard, key)
    Invulnerable,  ///< hurt frames
    Removed,       ///< removed or dead
    Owner,         ///< tamed by another player
    Lists,         ///< entity black/whitelist (babies with `allowBabies`, override_checks bypass)
    Hostile,       ///< monster in survival without `pickupHostileMobs`
    Size,          ///< taller than `maxEntityHeight` or wider than `maxEntityWidth` in survival
    Condition,     ///< custom pickup condition not met
    Protection,    ///< a protection hook refused
    Script,        ///< `beforePickup` (script conditions) refused
};

[[nodiscard]] std::string_view resultName(Result result);

/// Runs the whole §3.2 pickup for `player` on `entity`: every check, then ejects passengers, stops
/// riding, drops the lead, captures and removes the entity, stores the carry, plays the sound, swings
/// and applies Slowness. Silent on refusal.
Result tryPickup(Player& player, Actor& entity);

/// Steps 1-8 of SPEC §3.2 only (no side effects): `PickedUp` means the pickup would pass them. Used
/// to offer the interaction to clients (see `Actor::getInteraction` in EntityPickup.cpp).
[[nodiscard]] Result precheck(Player& player, Actor& entity);

/// Test seam: the result of the last pickup attempt made by the interact hook (nullopt = none since
/// the last call), cleared by reading it.
[[nodiscard]] std::optional<Result> takeLastHookResult();

} // namespace lo::feature::entitypickup
