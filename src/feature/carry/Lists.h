#pragma once

// Carry On's black/whitelists and placement state exceptions, compiled from `CarryConfig` at start
// and on /carryon reload (docs/SPEC.md §3.13, §5.3). Server thread.
//
//   if (!carry::isBlockAllowed(block)) return;           // U1
//   if (!carry::isEntityAllowed(actor)) ...              // U3
//   if (!carry::isStackingAllowed(top)) return;          // U4
//   if (carry::isPlacementException("facing_direction")) // U2

#include <optional>
#include <string>
#include <string_view>

#include "core/CarryLists.h"

class Actor;
class Block;

namespace lo::feature::carry {

/// Block id + block tags + block states of `block` as a list candidate.
[[nodiscard]] core::carry::Candidate blockCandidate(Block const& block);
/// Entity type id + type families of `actor` as a list candidate.
[[nodiscard]] core::carry::Candidate entityCandidate(Actor const& actor);

/// A state of `block` as text ("true"/"false", decimal ints, strings), nullopt if it has none.
[[nodiscard]] std::optional<std::string> blockStateValue(Block const& block, std::string_view name);

[[nodiscard]] bool isBlockAllowed(Block const& block);
[[nodiscard]] bool isEntityAllowed(Actor const& actor);
[[nodiscard]] bool isStackingAllowed(Actor const& actor);
/// Id-only variants (no tags, no states), e.g. for commands and tests.
[[nodiscard]] bool isBlockIdAllowed(std::string_view id);
[[nodiscard]] bool isEntityIdAllowed(std::string_view id);

/// A state named by `placementStateExceptions` (recomputed on placement for every block having it).
[[nodiscard]] bool isPlacementException(std::string_view stateName);

/// Recompiles the lists from the current config (called on load and /carryon reload).
void rebuildLists();

} // namespace lo::feature::carry
