#pragma once

// Extension points of the carry features (docs/SPEC.md §3.10–§3.12, §4.4, §6). Units and other mods
// plug in here instead of editing the pickup / placement units.
//
// 1. Pickup hooks (scripts U8, custom pickup conditions U8, land-protection plugins):
//
//        LO_REGISTER_PICKUP_HOOKS(lo::feature::carry::PickupHooks{
//            .name           = "scripts",
//            .overrideChecks = [](PickupTarget const& t) { return ...; },
//            .beforePickup   = [](PickupTarget const& t, CarryData& data) { data.activeScript = "x"; return true; },
//        });
//
//    The pickup units call the stages in Carry On's order (SPEC §3.1 / §3.2):
//        overrideChecks(t)   -> skip the overridable checks (script `override_checks`)
//        customConditions(t) -> false refuses (custom pickup conditions)
//        protectionAllows(t) -> false refuses (built-in LeviLamina event + hooks)
//        beforePickup(t, d)  -> false refuses; may fill d.activeScript / d.scriptRender, runs commandPickup
//    The placement units call placeAllowed(...) and runOnPlace(...); the spine calls `onLoop` every
//    server tick for every carrier.
//
// 2. Placement state resolver (U2): how a carried block is oriented when placed. `placeCarried`
//    (death / command placement) uses it too.
//
// 3. Visual providers (U12 model overrides + script render; read by U10/U11/U13): what is drawn for a
//    carry. `visualFor(data)` = the carried thing as is, then every provider in priority order.
//
// 4. Debug line providers (U8, U12; printed by U7's `/carryon debug`).
//
// Everything runs on the server thread, except visual providers (client thread on clients; server
// thread for U13's vanilla stand-ins).

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mod/Registry.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lo::feature::carry {

struct CarryData;

// ---- 1. pickup / placement hooks --------------------------------------------------------------

/// What a pickup attempt targets: a block (`block`, `pos`, `blockEntity`) or an entity (`entity`).
struct PickupTarget {
    Player&            player;
    BlockSource&       region;
    Block const*       block       = nullptr;
    BlockPos const*    pos         = nullptr;
    std::string const* blockEntity = nullptr; ///< SNBT of the block actor ("" or nullptr = none)
    Actor*             entity      = nullptr;

    [[nodiscard]] bool isBlock() const { return block != nullptr; }
};

/// A placement attempt: the carried block at `pos`, or the carried entity / player placed at `pos`.
/// `face` = clicked face (0 down, 1 up, 2 north, 3 south, 4 west, 5 east).
struct PlaceTarget {
    Player&          player;
    BlockSource&     region;
    BlockPos const&  pos;
    CarryData const& data;
    int              face = 1;
};

/// One set of hooks. Every member is optional (empty = no opinion).
struct PickupHooks {
    std::string name;
    int         priority = 0; ///< higher runs first

    /// True skips Carry On's overridable checks (unbreakable, block entity required, entity lists,
    /// hostile mobs). Any hook returning true wins.
    std::function<bool(PickupTarget const&)> overrideChecks;
    /// Custom pickup conditions: false refuses.
    std::function<bool(PickupTarget const&)> customCondition;
    /// Protection (land claims): false refuses.
    std::function<bool(PickupTarget const&)> protection;
    /// Last step before the pickup happens (script conditions + activation + commandPickup): false
    /// refuses; may write `data.activeScript` / `data.scriptRender`.
    std::function<bool(PickupTarget const&, CarryData& data)> beforePickup;
    /// Placement protection (blocks and entities; not stacking / death placement): false refuses.
    std::function<bool(PlaceTarget const&)> placeProtection;
    /// After a successful placement / stacking, before the data is cleared (commandPlace).
    std::function<void(Player&, CarryData const&)> onPlace;
    /// Every server tick for every carrier (commandLoop).
    std::function<void(Player&, CarryData const&)> onLoop;
};
using PickupHookRegistry = StaticRegistry<PickupHooks>;

/// Stage aggregates, in Carry On's order. `overrideChecks`: any hook; the others: every hook must agree.
[[nodiscard]] bool overrideChecks(PickupTarget const& target);
[[nodiscard]] bool customConditions(PickupTarget const& target);
/// Built-in: for blocks publishes a cancellable LeviLamina `PlayerDestroyBlockEvent` (inside
/// `inProtectionCheck()`), cancelled = refused; then the hooks' `protection`.
[[nodiscard]] bool protectionAllows(PickupTarget const& target);
[[nodiscard]] bool beforePickup(PickupTarget const& target, CarryData& data);
/// Built-in for block placement: publishes `PlayerPlacingBlockEvent` (inside `inProtectionCheck()`);
/// then the hooks' `placeProtection`. `block`: true for a block placement (entities: hooks only).
[[nodiscard]] bool placeAllowed(PlaceTarget const& target, bool block);
/// Every hook's `onPlace` (commandPlace).
void runOnPlace(Player& player, CarryData const& data);

/// True while the spine publishes a synthetic protection event: units that cancel such events for
/// carriers (U6: no breaking while carrying) must ignore them.
[[nodiscard]] bool inProtectionCheck();

/// Registers hooks (at static init). Put it in a .cpp file.
#define LO_REGISTER_PICKUP_HOOKS(...)                                                                                  \
    static ::lo::feature::carry::detail::PickupHooksRegistrar LO_UNIQUE(lo_pickup_hooks_)(__VA_ARGS__)

// ---- 2. placement state ---------------------------------------------------------------------

/// Computes the state a carried block takes when placed at `target` (SPEC §3.6 step 3): `stored` is
/// the carried state, `face` the clicked face, `clickPos` the hit position (world coordinates).
/// Returns nullptr to use the stored state.
using PlacementStateResolver = std::function<Block const*(
    Player& player, BlockSource& region, BlockPos const& target, Block const& stored, int face, Vec3 const& clickPos
)>;
/// Sets the resolver (U2, in `install()`); the default keeps the stored state.
void setPlacementStateResolver(PlacementStateResolver fn);
[[nodiscard]] Block const& resolvePlacementState(
    Player& player, BlockSource& region, BlockPos const& target, Block const& stored, int face, Vec3 const& clickPos
);

// ---- 3. visuals -----------------------------------------------------------------------------

/// What to draw for a carry.
struct CarryVisual {
    enum class Kind { None, Block, Item, Entity };
    Kind        kind = Kind::None;
    std::string block;  ///< Kind::Block: block state SNBT (drawn as the block's item)
    std::string item;   ///< Kind::Item: item id
    std::string entity; ///< Kind::Entity: entity SNBT (display actor)
    /// Set when a model override replaced the block by an item (renderHeight 0.5, renderWidth 0.8).
    bool itemOverride = false;

    /// Script transform (rotation in degrees), applied after the base transform.
    std::array<float, 3> translation{0.f, 0.f, 0.f};
    std::array<float, 3> rotation{0.f, 0.f, 0.f};
    std::array<float, 3> scale{1.f, 1.f, 1.f};
    /// Arm rotations in radians replacing the computed value per axis (nullopt = computed).
    std::array<std::optional<float>, 3> leftArm, rightArm;
    bool                                renderLeftArm  = true; ///< false = vanilla pose for that arm
    bool                                renderRightArm = true;
};

/// May modify `visual` (it starts as the carried thing as is).
struct VisualProvider {
    std::string                                                     name;
    int                                                             priority = 0; ///< higher runs first
    std::function<void(CarryData const& data, CarryVisual& visual)> apply;
};
using VisualProviderRegistry = StaticRegistry<VisualProvider>;

/// The visual of `data`: the carried block / entity, then every provider.
[[nodiscard]] CarryVisual visualFor(CarryData const& data);

#define LO_REGISTER_VISUAL_PROVIDER(...)                                                                               \
    static ::lo::feature::carry::detail::VisualProviderRegistrar LO_UNIQUE(lo_visual_provider_)(__VA_ARGS__)

// ---- 4. debug lines -------------------------------------------------------------------------

/// Adds lines to `/carryon debug` (e.g. "Override Model: ...", "Custom Pickup Condition: ...").
using DebugLineProvider =
    std::function<void(Player const& player, CarryData const& data, std::vector<std::string>& lines)>;
using DebugLineProviderRegistry = StaticRegistry<DebugLineProvider>;
/// Runs every provider (U7 calls it after its own lines).
[[nodiscard]] std::vector<std::string> extraDebugLines(Player const& player, CarryData const& data);

#define LO_REGISTER_DEBUG_LINES(...)                                                                                   \
    static ::lo::feature::carry::detail::DebugLineRegistrar LO_UNIQUE(lo_debug_lines_)(__VA_ARGS__)

namespace detail {
struct PickupHooksRegistrar {
    explicit PickupHooksRegistrar(PickupHooks hooks) { PickupHookRegistry::add(std::move(hooks)); }
};
struct VisualProviderRegistrar {
    explicit VisualProviderRegistrar(VisualProvider p) { VisualProviderRegistry::add(std::move(p)); }
};
struct DebugLineRegistrar {
    explicit DebugLineRegistrar(DebugLineProvider p) { DebugLineProviderRegistry::add(std::move(p)); }
};
} // namespace detail

} // namespace lo::feature::carry
