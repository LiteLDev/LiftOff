#pragma once

// Model overrides (`carryon_client.json` `modelOverrides`) and script render data (`CarryData::scriptRender`),
// game independent. See docs/SPEC.md §3.12 (render), §4.4, §5.2.
//
//   auto o = lo::core::modeloverride::parse("minecraft:chest{Findable:1b}->(block)minecraft:barrel[facing_direction=1]");
//   o->source      // ListEntry minecraft:chest (wildcards, #tags and [state=value] filters as in the lists)
//   o->nbt         // "{Findable:1b}"  (partial compare against the block actor NBT, done by the caller)
//   o->kind        // TargetKind::Block (default Item)
//   o->target      // "minecraft:barrel", o->targetStates = {facing_direction=1}
//
//   auto r = lo::core::modeloverride::parseScriptRender(data.scriptRender);
//   r.nameBlock, r.translation, r.leftArm[0] ...

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/CarryLists.h"

namespace lo::core::modeloverride {

enum class TargetKind { Item, Block };

/// One `modelOverrides` entry: `<block>[state=value,...]{snbt}->(item|block)<target>[state=value,...]`.
struct ModelOverride {
    std::string     text;   ///< the entry as written (trimmed)
    carry::ListEntry source; ///< id / wildcard / #tag, with `[state=value]` filters
    std::string     nbt;    ///< SNBT compound `{...}` ("" = no NBT condition)
    TargetKind      kind = TargetKind::Item;
    std::string     target;                   ///< target item / block id (namespaced)
    std::vector<carry::StateFilter> targetStates; ///< block target `[state=value]` (always with values)

    /// The target as written in normalised form: `(item)minecraft:redstone`, `(block)minecraft:cake[a=1]`.
    [[nodiscard]] std::string targetText() const;
};

/// Adds `minecraft:` to an id without a namespace ("stone" -> "minecraft:stone"); leaves "" alone.
[[nodiscard]] std::string withNamespace(std::string_view id);

/// Parses one entry. nullopt for malformed entries: no `->`, empty / invalid source or target,
/// unbalanced `{}` / `[]`, state filters without a value, a wildcard / tag / NBT on the target,
/// `[...]` on an item target, unknown `(kind)`.
[[nodiscard]] std::optional<ModelOverride> parse(std::string_view text);

/// Parses every entry in order (first match wins). Invalid entries are skipped; their text is appended
/// to `invalid` when given.
[[nodiscard]] std::vector<ModelOverride>
parseAll(std::vector<std::string> const& entries, std::vector<std::string>* invalid = nullptr);

/// `id[state=value,...]` (a block state spec as in a block target or script `name_block`). nullopt when
/// the id is empty / contains '*' or '#', brackets are unbalanced or a state has no value.
struct BlockSpec {
    std::string                     id; ///< namespaced
    std::vector<carry::StateFilter> states;
};
[[nodiscard]] std::optional<BlockSpec> parseBlockSpec(std::string_view text);

// ---- script render --------------------------------------------------------------------------

using Axes = std::array<std::optional<float>, 3>;

/// "x=1,y=0.5,z=-2" -> {1, 0.5, -2}. Missing or malformed axes are nullopt; keys are x / y / z
/// (case-insensitive), whitespace is ignored, the last value of a repeated key wins.
[[nodiscard]] Axes parseAxes(std::string_view text);
/// `parseAxes` with the missing axes set to `fallback`.
[[nodiscard]] std::array<float, 3> parseVector(std::string_view text, float fallback);

/// The `render` object of a script (SPEC §3.12).
struct ScriptRender {
    std::optional<std::string> nameBlock;  ///< `name_block`: block spec text (namespaced id + states)
    std::optional<std::string> nameEntity; ///< `name_entity`: entity type id (namespaced)
    std::optional<std::string> nbt;        ///< `nbt` as SNBT compound text
    std::array<float, 3>       translation{0.f, 0.f, 0.f};
    std::array<float, 3>       rotation{0.f, 0.f, 0.f}; ///< degrees
    std::array<float, 3>       scale{1.f, 1.f, 1.f};
    Axes                       leftArm, rightArm; ///< radians, nullopt = computed
    bool                       renderLeftArm  = true;
    bool                       renderRightArm = true;

    /// True when the render object changes anything at all.
    [[nodiscard]] bool empty() const;
};

/// Reads a script `render` object. Never throws: wrong types / a non-object are ignored (defaults).
/// `nbt` may be a JSON object (converted with `jsonToSnbt`) or an SNBT string. Empty strings count as
/// absent.
[[nodiscard]] ScriptRender parseScriptRender(nlohmann::json const& render);

/// JSON value -> SNBT text: objects -> compounds, arrays -> lists, strings -> quoted strings, booleans ->
/// bytes (`1b`), integers -> ints (`L` suffix when outside the int range), other numbers -> doubles (`d`
/// suffix), null -> `0b`.
[[nodiscard]] std::string jsonToSnbt(nlohmann::json const& value);

} // namespace lo::core::modeloverride
