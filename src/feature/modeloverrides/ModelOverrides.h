#pragma once

// U12: model overrides (`carryon_client.json` `modelOverrides`, SPEC §5.2) and script render data
// (`CarryData::scriptRender`, SPEC §3.12) turned into the `CarryVisual` drawn for a carry (SPEC §4.4).
//
// The renderers never call this directly: they call `carry::visualFor(data)`, which runs the visual
// provider registered here. Order inside the provider:
//   1. block carry: script `name_block` replaces the block; otherwise the first matching model override
//      replaces it (item target: Kind::Item + itemOverride; block target: Kind::Block, drawn as its item);
//   2. entity carry: script `name_entity` (a fresh entity of that type) and `nbt` (merged into the
//      rendered entity's NBT);
//   3. script transform (translation / rotation / scale) and arm values.
// Model overrides are client settings: they apply only in the client build (on BDS there is no client
// config, as on a Java dedicated server). Script render applies on both (U13's vanilla stand-ins).
//
// Thread-safe: the provider runs on the client thread (renderers) and the server thread (stand-ins,
// `/carryon debug`); the compiled list is swapped atomically on `/carryon reload`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mc/deps/nbt/CompoundTag.h"

#include "core/ModelOverride.h"
#include "feature/carry/Hooks.h"

class Block;
class Tag;

namespace lo::feature::carry {
struct CarryData;
}

namespace lo::feature::modeloverrides {

/// A parsed entry with its NBT condition parsed.
struct CompiledOverride {
    core::modeloverride::ModelOverride entry;
    std::optional<CompoundTag>         nbt; ///< nullopt = no NBT condition (none, or `{}`)
};

/// Parses `entries` in order; invalid ones (syntax or SNBT) are skipped and appended to `invalid`.
[[nodiscard]] std::vector<CompiledOverride>
compile(std::vector<std::string> const& entries, std::vector<std::string>* invalid = nullptr);

/// Minecraft's partial NBT compare: every key of `expected` exists in `actual` with a matching value
/// (recursively); a non-empty expected list matches when each of its elements matches some element of
/// the actual list, an empty one only an empty list; other tags must be equal (same type and value).
[[nodiscard]] bool nbtPartialMatch(Tag const& expected, Tag const& actual);

/// The block `id` with `states` set (values as text, typed by the block's own states). nullptr when
/// the block or one of the states is unknown or a value does not fit.
[[nodiscard]] Block const* resolveBlock(std::string_view id, std::vector<core::carry::StateFilter> const& states);
/// `resolveBlock` from spec text (`id[state=value]`), as block state SNBT ("" when unresolvable).
[[nodiscard]] std::string resolveBlockSnbt(std::string_view spec);

/// What a matching override draws.
struct OverrideResult {
    std::size_t              index = 0; ///< position in the list
    std::string              target;    ///< `ModelOverride::targetText()` (debug line)
    carry::CarryVisual::Kind kind = carry::CarryVisual::Kind::Item;
    std::string              block; ///< Kind::Block: target block state SNBT
    std::string              item;  ///< Kind::Item: target item id
};

/// First entry of `list` matching `block` (id / tag / states) and `blockEntity` (NBT condition; an
/// entry with one never matches a block without block actor) whose target exists.
[[nodiscard]] std::optional<OverrideResult>
findOverride(std::vector<CompiledOverride> const& list, Block const& block, CompoundTag const* blockEntity);

/// True in the client build: model overrides are client settings (a dedicated server has none).
[[nodiscard]] bool modelOverridesActive();
/// `findOverride` with the configured list, from block state SNBT + block actor SNBT (cached). Does not
/// check `modelOverridesActive()`.
[[nodiscard]] std::optional<OverrideResult> configuredOverride(std::string const& blockSnbt, std::string const& blockEntitySnbt);

/// Entity SNBT drawn for `entitySnbt` with script render `nameEntity` / `nbt`.
[[nodiscard]] std::string renderEntitySnbt(
    std::string const&                entitySnbt,
    std::optional<std::string> const& nameEntity,
    std::optional<std::string> const& nbt
);

/// The provider body: applies model overrides (`useModelOverrides`) and script render to `visual`.
void applyVisual(carry::CarryData const& data, carry::CarryVisual& visual, bool useModelOverrides);

/// Re-reads `CarryClientConfig::modelOverrides` (called at install and on `/carryon reload`).
void rebuild();

} // namespace lo::feature::modeloverrides
