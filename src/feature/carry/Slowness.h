#pragma once

// Carry On's hidden Slowness while carrying (docs/SPEC.md §3.4). Server thread.
//
//   carry::applySlowness(player, carry::blockAmplifier(carry::get(player)));   // after a block pickup
//   carry::removeSlowness(player);                                              // after placing / clearing

class Actor;
class Player;

namespace lo::feature::carry {

struct CarryData;

/// Slowness effect id (Bedrock `MobEffectIds::MovementSlowdown`).
inline constexpr int kSlownessEffectId = 2;
/// Duration Carry On uses (effectively infinite).
inline constexpr int kSlownessDuration = 100000000;

/// Block: from `data.nbtSize()`, `heavyTiles`, `blockSlownessMultiplier`.
[[nodiscard]] int blockAmplifier(CarryData const& data);
/// Entity: from the entity's collision box, `heavyEntities`, `relativePlayerScale` (carrier scale),
/// `entitySlownessMultiplier`.
[[nodiscard]] int entityAmplifier(Actor const& entity, Player const& carrier);
/// Player: always 1 (Slowness II).
[[nodiscard]] int playerAmplifier();

/// Entity collision box height / width (Bedrock AABB dimensions).
[[nodiscard]] float entityHeight(Actor const& entity);
[[nodiscard]] float entityWidth(Actor const& entity);
/// The carrier's scale (1 unless changed by an add-on / command).
[[nodiscard]] float playerScale(Player const& player);

/// Adds Slowness `amplifier` (0 = I .. 4 = V) for 100 000 000 ticks, not ambient, no particles.
/// Does nothing in creative unless `slownessInCreative`.
void applySlowness(Player& player, int amplifier);
/// Removes every Slowness effect (also from other sources, like Carry On).
void removeSlowness(Player& player);
/// The current Slowness amplifier, -1 without Slowness (selftests, debug).
[[nodiscard]] int slownessAmplifier(Player const& player);

} // namespace lo::feature::carry
