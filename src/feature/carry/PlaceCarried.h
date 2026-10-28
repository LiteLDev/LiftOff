#pragma once

// Carry On's forced placement (death, `/carryon place`, dropCarriedWhenHit, carried player leaving):
// docs/SPEC.md §3.5. Server thread.
//
//   carry::placeCarried(player);                       // at the player's position
//   carry::placeCarriedAt(player, region, deathPos);   // at a remembered position (U7, death)

#include <optional>
#include <string>

class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lo::feature::carry {

struct CarryData;

struct PlaceResult {
    bool        placed = false; ///< something was carried and has been placed / released
    std::string what;           ///< "block minecraft:chest", "entity minecraft:pig", "player Steve"
    int         x = 0, y = 0, z = 0;
};

/// Places whatever `player` carries and clears the carry + removes Slowness:
/// - block: placement state from the resolver (face down, click at the player's block position); the
///   first position of the cube -14..+14 around the player's block position (sorted by distance)
///   where the block survives, the target is replaceable and no entity obstructs; fallback: the
///   player's own block position, overwriting it. Block actor restored. No event, no sound.
/// - entity: recreated (same UniqueID) at the position.
/// - player: the carried player is ejected (StartRidingOther false to modded clients).
/// Returns `placed = false` when the player carries nothing.
PlaceResult placeCarried(Player& player);
/// The same at `pos` in `region` (e.g. the death position and dimension).
PlaceResult placeCarriedAt(Player& player, BlockSource& region, Vec3 const& pos);

/// Carry On's "can place here" test shared by the placement units: `block` can survive at `pos`,
/// `pos` is inside the height range and replaceable, and no entity intersects the block's collision
/// box.
[[nodiscard]] bool canPlaceBlockAt(BlockSource& region, BlockPos const& pos, Block const& block);

/// True when an entity's collision box blocks `block` at `pos` (empty collision shapes never do).
[[nodiscard]] bool isObstructed(BlockSource& region, BlockPos const& pos, Block const& block);

} // namespace lo::feature::carry
