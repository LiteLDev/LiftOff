#pragma once

// Block helpers (server thread): typed custom states, placement, positions.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"

class Block;
class BlockPos;
class BlockSource;
class Vec3;
class Player;
class Actor;

namespace lo::game {

/// The block's full id ("minecraft:stone", "liftoff:example", ...).
[[nodiscard]] std::string blockId(Block const& block);
/// The block at `pos` ("" when the chunk isn't loaded).
[[nodiscard]] std::string blockIdAt(BlockSource& region, BlockPos const& pos);

/// Integer / boolean custom state of `block` (nullopt if the block has no such state).
[[nodiscard]] std::optional<int> intState(Block const& block, std::string_view state);

/// The default state of `id` with `states` applied (unknown states are ignored). nullptr if the
/// block id is not registered (behavior pack missing).
[[nodiscard]] Block const* blockWith(std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {});

/// Sets a block (neighbour + client updates). Returns false on failure.
bool setBlock(BlockSource& region, BlockPos const& pos, Block const& block);
/// Replaces the block with air.
bool clearBlock(BlockSource& region, BlockPos const& pos);

/// The block at `pos` can be replaced by a placement (air, grass, water, ...).
[[nodiscard]] bool replaceable(BlockSource& region, BlockPos const& pos);

/// Java horizontal facing (0 S, 1 W, 2 N, 3 E) the player looks along; a placed block faces the
/// opposite (towards the player).
[[nodiscard]] int playerFacing(Actor const& actor);
[[nodiscard]] inline int opposite(int facing) { return (facing + 2) & 3; }
/// Unit offset (dx, dz) of a Java 2D facing.
[[nodiscard]] std::pair<int, int> facingOffset(int facing);

[[nodiscard]] core::GlobalPos globalPos(BlockSource const& region, BlockPos const& pos);
[[nodiscard]] core::GlobalPos globalPos(int dim, BlockPos const& pos);
[[nodiscard]] BlockPos        blockPos(core::GlobalPos const& pos);
/// The block a command position lies in: rounded down, so -4.5 is block -5. (The SDK's inline
/// CommandPosition::getBlockPos assigns floats to ints, which rounds toward zero and puts every
/// negative coordinate one block off.)
[[nodiscard]] BlockPos        blockPosOf(Vec3 const& pos);

} // namespace lo::game
