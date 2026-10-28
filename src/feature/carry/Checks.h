#pragma once

// Checks shared by several units (docs/SPEC.md §3.1, §3.2). Server thread.

#include <string_view>

class Actor;
class Block;
class Player;
class Vec3;
enum class GameType : int;

namespace lo::feature::carry {

/// The player's effective game mode (`Default` resolved to the world's default).
[[nodiscard]] GameType gameType(Player const& player);
[[nodiscard]] bool     isCreative(Player const& player);

/// The generic pickup preconditions of Carry On, shared by block (U1) and entity (U3) pickup, in
/// Carry On's order:
///   1. distance: `distSq(player feet, target) < maxDistance^2 * scale^2` (scale: SPEC §3.1/§3.2)
///   2. game mode not Adventure / Spectator; main hand and offhand empty; not carrying; tick guard
///      (`canAct`); carry key pressed (`keyPressed`).
/// `target`: block centre (blocks) or entity position (entities).
[[nodiscard]] bool canStartPickup(Player const& player, Vec3 const& target, bool entity);

/// Double-height blocks are never picked up: the `upper_block_bit` state (doors, tall flowers,
/// pitcher plant, small dripleaf, ...) or tall seagrass (`sea_grass_type` != default).
[[nodiscard]] bool isDoubleHeight(Block const& block);

/// Block actors that exist only on Bedrock (Java has no block entity for these blocks): they do not
/// count for Carry On's "needs a block entity" rule (SPEC §3.1 step 8).
[[nodiscard]] bool isBedrockOnlyBlockActor(std::string_view blockId);

/// Destroy speed < 0 (bedrock, barrier, command blocks, ...).
[[nodiscard]] bool isUnbreakable(Block const& block);

} // namespace lo::feature::carry
