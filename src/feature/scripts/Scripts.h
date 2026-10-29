#pragma once

// Carry On scripts and custom pickup conditions (docs/SPEC.md §3.10, §3.12). Server thread.
//
// Scripts live in `config/scripts/**/*.json` (id = relative path without ".json"), loaded at start and
// on /carryon reload. Everything is wired into the pickup units through `LO_REGISTER_PICKUP_HOOKS`
// (overrideChecks, customCondition, beforePickup, onPlace, onLoop); other code rarely needs this header
// except for tests.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ScriptMatch.h"

class Actor;
class Block;
class Player;

namespace lo::feature::carry {
struct PickupTarget;
}

namespace lo::feature::scripts {

using core::scripts::CustomCondition;
using core::scripts::Script;

// ---- scripts ---------------------------------------------------------------------------------

/// Reloads `config/scripts/**/*.json` (logs `scripts: loaded <N> script(s), <M> invalid`).
void reloadScripts();
/// The loaded scripts, highest priority first.
[[nodiscard]] std::vector<Script> const& scripts();
/// Replaces the loaded scripts (sorted by priority); returns the previous ones (selftests).
std::vector<Script> exchangeScripts(std::vector<Script> scripts);
/// The loaded script with this id (nullptr if none).
[[nodiscard]] Script const* findScript(std::string_view id);

/// First script whose object matches the block / entity (ignores `useScripts`; nullptr = none).
[[nodiscard]] Script const* matchBlock(Block const& block, std::string_view blockEntitySnbt);
[[nodiscard]] Script const* matchEntity(Actor& entity);
/// First matching script of a pickup target, or nullptr (always nullptr when `useScripts` is false).
[[nodiscard]] Script const* matchTarget(carry::PickupTarget const& target);

/// Every condition of `script` holds for `player`.
[[nodiscard]] bool conditionsMet(Script const& script, Player& player);
/// The player's score in `objective` (nullopt: no such objective or no score).
[[nodiscard]] std::optional<int> playerScore(Player const& player, std::string const& objective);
/// Runs `execute as "<player>" run <command>` from the server origin (full permissions); "" = nothing.
/// Returns false when the command failed.
bool runAsPlayer(Player& player, std::string_view command);

// ---- custom pickup conditions -----------------------------------------------------------------

/// Recompiles `customPickupConditionsBlocks/Entities` from the config.
void rebuildConditions();
/// The first custom condition matching the block / entity (nullptr = none).
[[nodiscard]] CustomCondition const* blockCondition(Block const& block);
[[nodiscard]] CustomCondition const* entityCondition(Actor const& entity);
/// Entity condition by type id only (carried entities no longer exist as actors, so `#family` entries
/// cannot match here; used for the debug line).
[[nodiscard]] CustomCondition const* entityConditionById(std::string_view id);
/// The player has the stage (tag `carryon:stage:<stage>`).
[[nodiscard]] bool hasStage(Player const& player, std::string_view stage);
/// The first matching condition of the target is met (true when none matches).
[[nodiscard]] bool customConditionMet(carry::PickupTarget const& target);

} // namespace lo::feature::scripts
