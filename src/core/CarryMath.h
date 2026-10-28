#pragma once

// Carry On numbers that do not need the game: Slowness amplifiers and the pickup reach.
// See docs/SPEC.md §3.1, §3.2, §3.4.

#include <cstddef>

namespace lo::core::carry {

/// Highest Slowness amplifier (Slowness V).
inline constexpr int kMaxSlowness = 4;
/// Amplifier while carrying a player (Slowness II), multipliers do not apply.
inline constexpr int kPlayerSlowness = 1;

/// Block: `heavyTiles ? nbtSize / 500 : 1`, then `(int)(i * multiplier)`, capped at 4 (never below 0).
[[nodiscard]] int blockSlowness(std::size_t nbtSize, bool heavyTiles, double multiplier);

/// Entity: `heavyEntities ? (int)(height * width * scaleFactor^2) : 1`, then `(int)(i * multiplier)`, capped
/// at 4. `scaleFactor` = 1 / carrier scale with `relativePlayerScale`, else 1. Computed in float like Java.
[[nodiscard]] int entitySlowness(float height, float width, float scaleFactor, bool heavyEntities, double multiplier);

/// Pickup reach scale: 1 unless `scaleDistance`; then interaction range / base range (Java 1.20.5+):
/// blocks 4.5 (+0.5 in creative), entities 3.0 (+2.0 in creative).
[[nodiscard]] double reachScale(bool scaleDistance, bool creative, bool entity);

/// `distSq < maxDistance^2 * scale^2`.
[[nodiscard]] bool withinReach(double distSq, double maxDistance, double scale);

} // namespace lo::core::carry
