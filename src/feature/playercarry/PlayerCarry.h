#pragma once

// Carrying players (docs/SPEC.md §3.2 step 12, §3.3, §3.7 step 3, §3.8 with a carried player).
// Server thread.
//
// The carried player really rides the carrier (forced link, sent to every client by the game).
// Bedrock players are rideable (parrots sit on their shoulders), so the carrier's own seats are
// re-targeted while carrying: every seat is moved to 0.6 blocks in front of the carrier and 0.4
// blocks above its feet, and locks the rider's yaw to ±30° of the carrier's. The game then positions
// the rider every tick (server and clients alike: the seat offset travels as actor data), smoothly
// and without teleports. The original seats come back when the carry ends.
//
// The functions below are what the hooks call; selftests use them directly.

#include <string>

class Actor;
class BlockPos;
class Player;
class Vec3;

namespace lo::feature::playercarry {

/// Seat geometry (Carry On: 0.6 forward along the carrier's body yaw, 0.4 above its feet; the
/// rider's yaw clamped to ±30° of the carrier's).
inline constexpr float kForward      = 0.6f;
inline constexpr float kUp           = 0.4f;
inline constexpr float kYawLimitDeg  = 30.0f;

/// §3.2 with a player target: all checks, then the pickup (step 12). True when `target` is now
/// carried by `carrier` (the caller cancels the vanilla interaction).
bool tryPickup(Player& carrier, Player& target);

/// §3.7 step 3: `carrier` clicked `clicked` (face 0..5) while carrying a player. The player is put
/// down at the bottom centre of the target block. False (lava pop) when no target block is free.
bool tryPlace(Player& carrier, BlockPos const& clicked, int face);

/// §3.8 with a carried player: `carrier` clicked `clicked`. True when the interaction was handled
/// (stacked or refused with a sound); the caller always cancels the vanilla interaction.
bool tryStack(Player& carrier, Actor& clicked);

/// The player `carrier` carries (nullptr: none / offline).
[[nodiscard]] Player* carriedPlayer(Player const& carrier);
/// The player carrying `player` (nullptr: nobody).
[[nodiscard]] Player* carrierOf(Player const& player);

/// Releases a carried player as Carry On does when the rider leaves (dismount, death, logout,
/// teleport): carry cleared, Slowness removed, seats restored, `start_riding(false)` to a modded
/// carrier. `reason` goes into the log line. No-op when `carrier` carries no player.
void release(Player& carrier, std::string const& reason);

} // namespace lo::feature::playercarry
