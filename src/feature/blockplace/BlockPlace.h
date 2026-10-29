#pragma once

// U2 blockplace: putting a carried block down with a right-click (docs/SPEC.md §3.6). Server thread.
//
// The module listens to use-block (`PlayerInteractBlockEvent`): while the player carries a block the
// interaction is always cancelled, and the placement runs on the next server tick (a cancelled
// interaction rolls back block changes made during it). It also installs the §3.6 placement state
// rules as the spine's resolver (`carry::setPlacementStateResolver`), so forced placement
// (`carry::placeCarried`) orients blocks the same way.
//
// The functions below are the building blocks, exposed for the selftests:
//
//   auto target = blockplace::placementTarget(region, clicked, face);
//   auto r      = blockplace::tryPlace(player, clicked, face);   // what the deferred click runs

#include <string_view>

class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lo::feature::blockplace {

/// True when placement recomputes the state `stateName` of the block `blockId`: a built-in Bedrock
/// direction / axis state (Java `Direction` / `Axis` properties) or a `placementStateExceptions` state.
[[nodiscard]] bool isRecomputedState(std::string_view blockId, std::string_view stateName);

/// The §3.6 placement state of `stored` at `target` (the resolver installed in the spine): the
/// block's vanilla placement state for `player`, `face` and `clickPos` (world coordinates); copies the
/// recomputed states from it onto the stored state. nullptr = use the stored state unchanged.
[[nodiscard]] Block const* placementState(
    Player&         player,
    BlockSource&    region,
    BlockPos const& target,
    Block const&    stored,
    int             face,
    Vec3 const&     clickPos
);

/// The clicked position if its block is replaceable, else its neighbour on `face`
/// (0 down, 1 up, 2 north, 3 south, 4 west, 5 east).
[[nodiscard]] BlockPos placementTarget(BlockSource& region, BlockPos const& clicked, int face);

/// The neighbour of `pos` on `face`.
[[nodiscard]] BlockPos offset(BlockPos const& pos, int face);

/// True when a water source is at `pos` (liquid or block layer).
[[nodiscard]] bool hasWaterSource(BlockSource& region, BlockPos const& pos);

/// Carry On's "can place" test for a player (§3.6 step 4): survives, the player may build there,
/// replaceable target (outside the height range counts as void air, like Java), no obstructing
/// entity (the player included).
[[nodiscard]] bool canPlace(Player& player, BlockSource& region, BlockPos const& target, Block const& state, int face);

enum class PlaceResult {
    NotCarryingBlock, ///< nothing happened
    TickGuard,        ///< the carry changed this tick
    CannotPlace,      ///< lava pop played
    Protected,        ///< vetoed by protection (silent)
    OutsideWorld,     ///< above / below the height range (silent)
    Failed,           ///< the block could not be set (carry kept)
    Placed,
};

/// The placement of a click on `clicked` / `face` (§3.6 steps 1-9), run immediately.
PlaceResult tryPlace(Player& player, BlockPos const& clicked, int face);

} // namespace lo::feature::blockplace
