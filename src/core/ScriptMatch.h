#pragma once

// Carry On scripts and custom pickup conditions, game independent (docs/SPEC.md §3.10, §3.12).
//
//   auto s = parseScript(json, "my/script", &error);   // validity rules, nullopt + error if invalid
//   sortScripts(list);                                 // priority, highest first (stable)
//   matchesBound(">=5", 7.0);                          // number bounds <=n >=n <n >n =n n
//   parseScore("kills>=3")  -> {objective, bound}
//   parsePosition("x=1,dx=5") / positionMatches(p, bx, by, bz)  (axis with base 0 ignored: Carry On quirk)
//   parseEffects("minecraft:speed#1,haste") / effectId("speed")  (Bedrock MobEffectIds)
//   parseSnbt("{a:1b,b:\"x\"}") -> json; nbtMatches(expected, actual)  (partial compare)
//   parseCustomCondition("minecraft:chest[facing=2](stage)")

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/CarryLists.h"

namespace lo::core::scripts {

// ---- number bounds ---------------------------------------------------------------------------

/// `<=n`, `>=n`, `<n`, `>n`, `=n` or `n` (equal). Doubles.
struct NumberBound {
    enum class Op { Le, Ge, Lt, Gt, Eq };
    Op     op    = Op::Eq;
    double value = 0.0;

    [[nodiscard]] bool test(double v) const;
};

/// Parses a bound; nullopt if the text is empty or not a bound.
[[nodiscard]] std::optional<NumberBound> parseBound(std::string_view text);
/// Carry On semantics: an empty condition always matches, a malformed one never does.
[[nodiscard]] bool matchesBound(std::string_view text, double value);

// ---- conditions ------------------------------------------------------------------------------

/// `objective<op>value` (op one of <=, >=, <, >, =).
struct ScoreCondition {
    std::string objective;
    NumberBound bound;
};
[[nodiscard]] std::optional<ScoreCondition> parseScore(std::string_view text);

/// `x=,y=,z=,dx=,dy=,dz=` (all optional, default 0).
struct PositionCondition {
    double x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = 0;
};
[[nodiscard]] std::optional<PositionCondition> parsePosition(std::string_view text);
/// Each axis: `base <= v <= base + d`; an axis whose base is 0 is not checked (Carry On quirk, SPEC §3.12).
[[nodiscard]] bool positionMatches(PositionCondition const& c, double x, double y, double z);

/// `x=..,y=..,z=..` with defaults for missing axes (render translation / rotation / scale).
[[nodiscard]] std::optional<std::array<double, 3>> parseXyz(std::string_view text, double def);

/// One `effect#amplifier` requirement (amplifier default 0).
struct EffectRequirement {
    std::string name; ///< as written, without "minecraft:"
    int         id        = -1; ///< Bedrock MobEffectIds value, -1 = no such effect on Bedrock (never matches)
    int         amplifier = 0;
};
/// Comma list of `effect#amp`; nullopt if an amplifier is not an integer.
[[nodiscard]] std::optional<std::vector<EffectRequirement>> parseEffects(std::string_view text);
/// Bedrock effect id of a Java or Bedrock effect name (with or without `minecraft:`); -1 if unknown.
[[nodiscard]] int effectId(std::string_view name);

/// Java game-mode id of a Bedrock `GameType` value (survival 0, creative 1, adventure 2, spectator 3).
[[nodiscard]] int javaGameModeId(int bedrockGameType);

// ---- NBT -------------------------------------------------------------------------------------

/// Parses SNBT (as written by LeviLamina's `toSnbt`) into JSON: compounds -> objects, lists and typed
/// arrays -> arrays, numbers (any suffix) -> numbers, `true`/`false` -> 1 / 0, strings -> strings.
[[nodiscard]] std::optional<nlohmann::json> parseSnbt(std::string_view snbt);
/// Partial compare (Carry On `NbtUtils.compareNbt(expected, actual, true)`): every key of an expected
/// object must match; every element of an expected list must match some element of the actual list
/// (an empty expected list needs an empty list); numbers compare by value (booleans as 1 / 0).
[[nodiscard]] bool nbtMatches(nlohmann::json const& expected, nlohmann::json const& actual);

// ---- scripts ---------------------------------------------------------------------------------

struct Script {
    std::string id; ///< path relative to config/scripts without ".json"
    std::int64_t priority       = 0;
    bool         overrideChecks = false;
    bool         isBlock        = false; ///< object.block (else object.entity)

    // object
    std::optional<carry::ListEntry> name; ///< nullopt = any
    std::string                     hardness, resistance;           ///< block bounds ("" = any)
    std::string                     health, height, width;          ///< entity bounds ("" = any)
    std::optional<nlohmann::json>   nbt;                            ///< partial NBT

    // conditions ("" = no condition)
    std::string gamestage, advancement, xp, gamemode, scoreboard, position, effects;

    nlohmann::json render; ///< the `render` object (null when absent)

    // effects ("" = none)
    std::string commandPickup, commandLoop, commandPlace;

    [[nodiscard]] bool hasConditions() const;
};

/// Parses and validates one script file. Valid iff exactly one of `object.block` / `object.entity` and
/// at least one of `conditions`, `render`, `effects` or `"override_checks": true`. `error` receives
/// the reason when nullopt is returned.
[[nodiscard]] std::optional<Script> parseScript(nlohmann::json const& j, std::string id, std::string* error = nullptr);
/// Stable sort by priority, highest first.
void sortScripts(std::vector<Script>& scripts);

// ---- custom pickup conditions -----------------------------------------------------------------

/// `id[state=value,...](stage)` (blocks) / `id(stage)` (entities).
struct CustomCondition {
    std::string      text; ///< the entry as configured (trimmed)
    carry::ListEntry entry;
    std::string      stage;
};
[[nodiscard]] std::optional<CustomCondition> parseCustomCondition(std::string_view text);

/// The player tag that stands for a Game Stages stage (SPEC §3.10).
[[nodiscard]] std::string stageTag(std::string_view stage);

} // namespace lo::core::scripts
