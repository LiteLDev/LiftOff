#pragma once

// U10: first-person rendering of the local player's carry (docs/SPEC.md §4.1, §4.2). Client build only.
//
// While the local player carries a block or an entity, the vanilla first-person hand and held item
// (`ItemInHandRenderer::renderFirstPerson`) are replaced by the carried thing drawn in view space
// with Carry On's transforms:
//   block:  scale 2.5; translate (0, -0.5, -1); facePlayer != isChest ? rotate 180 Y, -8 X : +8 X;
//           script transform; drawn as the item of the block's default state (centred on the origin)
//   entity: rotate 180 Y; scale 0.8; translate (0, -h - 0.2, w * 1.3 + 0.1); script transform;
//           rotate 90 Y if rotateEntitiesSideways; drawn as a client-only actor built from the NBT
// The visual comes from `carry::visualFor(data)` (U12 model overrides / script render). Carried
// players keep the vanilla hand (Carry On draws nothing for them in first person).

#ifdef LO_CLIENT

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "glm/mat4x4.hpp"

namespace lo::feature::carry {
struct CarryVisual;
}

namespace lo::client::firstperson {

/// `minecraft:chest`, `trapped_chest`, `ender_chest` and every copper chest (SPEC §4.1 `isChest`).
[[nodiscard]] bool isChest(std::string_view blockId);

/// The view-space transform of a carried block (SPEC §4.2), the item model centred on the origin.
[[nodiscard]] glm::mat4 blockTransform(bool facePlayer, bool chest, feature::carry::CarryVisual const& visual);
/// The view-space transform of a carried entity of height `h` and width `w` (feet at the origin).
[[nodiscard]] glm::mat4
entityTransform(float h, float w, bool rotateSideways, feature::carry::CarryVisual const& visual);
/// Java's view bobbing of the hand (GameRenderer#bobView) for the interpolated walk distance and bob.
[[nodiscard]] glm::mat4 viewBobTransform(float walkDist, float bob);
/// The script transform: rotate X, Y, Z (degrees); translate; scale.
void applyScriptTransform(glm::mat4& m, feature::carry::CarryVisual const& visual);

/// What the first-person renderer drew last (for selftests; any thread).
struct DrawStatus {
    enum class Kind { None, Block, Item, Entity };
    Kind          kind = Kind::None;
    std::string   what;       ///< item / entity id drawn ("" = nothing)
    std::uint64_t frames = 0; ///< frames drawn for the current carry
    bool          actorAlive = false; ///< a display actor is cached
};
[[nodiscard]] DrawStatus status();

} // namespace lo::client::firstperson

#endif // LO_CLIENT
