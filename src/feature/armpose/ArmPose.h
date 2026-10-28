#pragma once

// U13 armpose (docs/SPEC.md §4.5): Carry On's arm pose for every client and a stand-in of the carried
// thing for vanilla clients on a LiftOff server.
//
// - Arm pose: resource-pack animations (`animations/liftoff_carry.animation.json`, see
//   core/ArmPose.h). The player entity file is not touched: the server starts the animations on
//   every client with `AnimateEntityPacket` (like /playanimation); modded clients load the animation
//   file into their animation group themselves (src/client/armpose/).
// - Stand-in: vanilla viewers of a carrier get a client-only display actor that never exists on the
//   server: a carried block (or item visual) is a display block (a falling block that never falls,
//   the block's default state at Carry On's 0.6 scale; an invisible armor stand's worn item does not
//   render on vanilla clients), a
//   carried entity is an actor of its type built from its NBT, scaled like Carry On's third-person
//   view. It follows the carrier with `MoveActorAbsolutePacket`. A carried player rides the carrier
//   for real (U5) and needs no stand-in. The carrier's own client gets no stand-in (in first person it
//   would cover the view; SPEC: vanilla carriers see their arm pose only).
// - Modded viewers (`net::isModded`) get the arm pose but never a stand-in: they draw the carried thing natively (U10/U11).
//
// Everything below runs on the server thread.

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/ArmPose.h"

class Level;
class Player;

namespace lo::feature::carry {
struct CarryData;
struct CarryVisual;
} // namespace lo::feature::carry

namespace lo::feature::armpose {

/// `renderWidth` of a carry (SPEC §4.1): block shape width (0.8 with an item override), entity width
/// * 0.9 (sideways: w - 0.35 w^2), a carried player's width * 0.9. `entityWidth` = collision width of
/// the carried entity when known (< 0: derived from the NBT).
/// `level`: where an entity's box is measured (nullptr = server level; see `entityBox`).
[[nodiscard]] float renderWidth(
    carry::CarryData const&   data,
    carry::CarryVisual const& visual,
    bool                      sideways,
    float                     entityWidth = -1.0f,
    Level*                    level       = nullptr
);

/// The pose a carry gets: `visualFor(data)` arm values, `renderArms` as given.
[[nodiscard]] core::armpose::Pose
poseFor(carry::CarryData const& data, bool renderArms = true, bool sideways = false, Level* level = nullptr);

// ---- selftest seams ---------------------------------------------------------------------------

/// What the unit sent to a viewer: kind "animate" (name = animation, detail.controller), "add"
/// (name = actor type, detail.id / scale), "equip" (name = item), "move" (detail.id), "remove" (detail.id).
struct SentPacket {
    std::string    kind;
    std::string    name;
    nlohmann::json detail;
};
/// Treats `viewer` (e.g. a SimulatedPlayer) as a vanilla client and records the packets it would get.
void setTestViewer(Player const& viewer, bool on);
/// The recorded packets of a test viewer, oldest first; cleared on read.
[[nodiscard]] std::vector<SentPacket> takeTestPackets(Player const& viewer);
/// The stand-in actor id currently shown to `viewer` for `carrier` (0 = none).
[[nodiscard]] std::uint64_t standInShownTo(Player const& carrier, Player const& viewer);

} // namespace lo::feature::armpose
