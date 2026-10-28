#pragma once

// Turning blocks and entities into carry data and back (docs/SPEC.md §3.1, §3.2, §3.6, §3.7).
// Server thread. Works on BDS and on the client build's integrated server.
//
// Block pickup (U1):
//     auto cap = carry::captureBlock(region, pos);              // state + block actor NBT
//     carry::removeCarriedBlock(region, pos);                    // nothing drops
// Block placement (U2):
//     carry::restoreBlock(region, target, data, placementState, waterlog);
// Entity pickup (U3) / placement (U4):
//     auto nbt = carry::captureActor(actor); carry::removeCapturedActor(actor);
//     Actor* a = carry::spawnActor(region, pos, data.entity);

#include <optional>
#include <string>
#include <string_view>

#include "mc/deps/nbt/CompoundTag.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class Vec3;

namespace lo::feature::carry {

struct CarryData;

// ---- NBT text ---------------------------------------------------------------------------------

/// Compact SNBT of a tag (the format stored in `CarryData`).
[[nodiscard]] std::string toSnbt(CompoundTag const& tag);
/// Parses SNBT written by `toSnbt` (nullopt on error or for "").
[[nodiscard]] std::optional<CompoundTag> parseNbt(std::string_view snbt);

// ---- blocks -----------------------------------------------------------------------------------

struct CapturedBlock {
    std::string block;       ///< SNBT of the block state
    std::string blockEntity; ///< SNBT of the block actor ("" = none)
    [[nodiscard]] bool hasBlockEntity() const { return !blockEntity.empty(); }
};

/// Reads the block at `pos` (its state without any water: water lives in the liquid layer) and its
/// block actor (saved with id and the pickup x/y/z). nullopt if the chunk is not loaded.
[[nodiscard]] std::optional<CapturedBlock> captureBlock(BlockSource& region, BlockPos const& pos);

/// Removes a picked-up block without drops: a paired chest is unpaired first (the other half stays a
/// single chest with its own items), the container is emptied, the block becomes air — or the water it
/// held. Neighbours are updated. Any item that still drops at `pos` during the removal is deleted.
bool removeCarriedBlock(BlockSource& region, BlockPos const& pos);

/// The carried block state (nullptr when the data is not a block or the block is unknown).
[[nodiscard]] Block const* carriedBlock(CarryData const& data);
/// "minecraft:chest" from the carried block state ("" when not a block).
[[nodiscard]] std::string carriedBlockId(CarryData const& data);

/// Puts the carried block at `pos`: `state` (the placement state; nullptr = the stored state), its
/// block actor rebuilt from the stored NBT at the new position (x/y/z rewritten, chest pairing
/// dropped), chests re-paired with a matching neighbour, the block actor data sent to clients, water
/// put in the liquid layer when `waterlog`. Neighbour updates (flags 3). No sound, no event.
/// Returns false if the block could not be set.
bool restoreBlock(BlockSource& region, BlockPos const& pos, CarryData const& data, Block const* state = nullptr, bool waterlog = false);

// ---- entities ---------------------------------------------------------------------------------

/// The entity's full NBT (incl. identifier and UniqueID) as SNBT.
[[nodiscard]] std::string captureActor(Actor const& actor);
/// Removes a picked-up entity: passengers ejected, stops riding, lead dropped as an item (Carry On:
/// animals on a lead), then removed — not killed: no death, no drops, no death sound.
void removeCapturedActor(Actor& actor);
/// Recreates an entity from `snbt` at `pos` (feet position) in `region`'s dimension. The UniqueID and
/// all data are kept. Returns nullptr on failure.
Actor* spawnActor(BlockSource& region, Vec3 const& pos, std::string_view snbt);
/// The entity type id ("minecraft:pig") of entity SNBT ("" if unknown).
[[nodiscard]] std::string entityIdOf(std::string_view snbt);

} // namespace lo::feature::carry
