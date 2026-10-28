#pragma once

// Sounds and arm swing of Carry On (docs/SPEC.md §3.9). Server thread; everything is broadcast to
// the players near the position (vanilla and modded clients alike).

#include <string_view>

class Actor;
class Block;
class BlockPos;
class Player;
class Vec3;

namespace lo::feature::carry {

/// Bedrock sound names for Carry On's named sounds.
namespace sound {
inline constexpr std::string_view kArmorEquip = "armor.equip_generic"; ///< pickup of entities/players (1.0, 0.5)
inline constexpr std::string_view kLavaPop    = "liquid.lavapop";      ///< failed placement/stacking (0.5, 0.5)
inline constexpr std::string_view kHorseSaddle = "mob.horse.leather";  ///< entity stacked (0.5, 1.5)
} // namespace sound

enum class BlockSound { Hit, Place };

/// The block's own hit / place sound at the block centre (LevelSoundEvent Hit / Place with the block's
/// runtime id; Bedrock clients choose the sound from the block). `player` is the source.
void playBlockSound(Player& player, BlockPos const& pos, Block const& block, BlockSound which);

/// A named sound at a position of `dimension` with Carry On's volume / pitch (PlaySoundPacket).
void playSound(int dimension, Vec3 const& pos, std::string_view name, float volume, float pitch);
/// The same at an actor's position and dimension.
void playSound(Actor const& at, std::string_view name, float volume, float pitch);

/// A mob's ambient sound (entity placed).
void playAmbient(Actor& actor);

/// Swings the player's main hand (seen by everyone, the player included).
void swing(Player& player);

} // namespace lo::feature::carry
