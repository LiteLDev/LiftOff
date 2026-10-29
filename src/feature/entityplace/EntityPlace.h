#pragma once

// U4 entityplace: placing a carried entity (docs/SPEC.md §3.7) and entity stacking (§3.8).
// Server thread.
//
// The unit listens itself (use-block while carrying an entity, entity interact while carrying an
// entity). The functions below are public so that other units can reuse them, e.g. U5 for stacking
// a carried player:
//
//     // GameMode::$interact while carrying a player (U5):
//     if (carry::canAct(player)) entityplace::tryStack(player, clicked);
//
// Bedrock only lets an entity ride a vehicle that has seats (`minecraft:rideable`). Java lets any
// entity carry one passenger, so `ride()` emulates the link for seatless vehicles (cow, sheep,
// villager, ...): the rider is kept on top of the vehicle every tick (seat at the vehicle's height,
// like Java). `vehicleOf` / `firstPassengerOf` see both kinds of link.

#include <string_view>

class Actor;
class BlockPos;
class Player;

namespace lo::feature::entityplace {

/// Outcome of `tryStack`.
enum class StackResult {
    NotCarrying, ///< the carrier carries no entity / player (nothing done)
    Disabled,    ///< `stackableEntities` is false (nothing done)
    Full,        ///< the column is full (lava pop)
    OnSelf,      ///< the top entity is the held entity (nothing done)
    Forbidden,   ///< the top entity is not allowed by the stacking lists (nothing done, silent)
    TooBig,      ///< `entitySizeMattersStacking`: the held entity is bigger than the top one (lava pop)
    Failed,      ///< the carried entity could not be recreated (the carry is kept)
    Stacked,     ///< stacked; the carry is cleared
};
[[nodiscard]] std::string_view stackResultName(StackResult r);

/// SPEC §3.8 for whatever `carrier` carries: an entity (recreated from its NBT) or a player (the
/// carrier's first passenger), stacked on the column of `clicked`. On success: `commandPlace`, swing,
/// carry cleared, sound `mob.horse.leather` (0.5, 1.5), Slowness removed, log line. Does not check
/// the tick guard (callers do: `carry::canAct`).
StackResult tryStack(Player& carrier, Actor& clicked);

/// SPEC §3.7 for an entity carry: `clicked` / `face` (0 down .. 5 east) of a use-block. Places the
/// entity at the bottom centre of the clicked block (or its neighbour on `face` when the clicked
/// block is not replaceable); lava pop when that is not replaceable either. Returns true when placed.
/// Does not check the tick guard. Carried players are not handled here (U5).
bool tryPlaceEntity(Player& carrier, BlockPos const& clicked, int face);

/// The vehicle of `actor`: the real one, or the emulated one (nullptr: not riding).
[[nodiscard]] Actor* vehicleOf(Actor const& actor);
/// The first passenger of `actor`: the real one, or the emulated one (nullptr: none).
[[nodiscard]] Actor* firstPassengerOf(Actor const& actor);
/// The bottom of the column `actor` is in (follows `vehicleOf`).
[[nodiscard]] Actor& rootVehicleOf(Actor& actor);
/// `rider` starts riding `vehicle`: a real (forced) link when the vehicle has seats, else an emulated
/// one (not for players). Returns false when neither worked (the rider stays where it is).
bool ride(Actor& rider, Actor& vehicle);
/// The emulated link of `rider` (no-op when none); the rider stays where it is.
void stopEmulatedRide(Actor const& rider);
/// `rider` rides `vehicle` through an emulated link.
[[nodiscard]] bool isEmulatedRide(Actor const& rider, Actor const& vehicle);

} // namespace lo::feature::entityplace
