#pragma once

// U1: block pickup (docs/SPEC.md §3.1). Server thread.
//
// Trigger: use-block (PlayerInteractBlockEvent: LeviLamina's GameMode::useItemOn hook, also called
// for an empty hand). Steps 1-11 run inside the event; when they pass, the interaction is cancelled
// (the block is not opened) and the rest runs on the next server tick, because a cancelled
// interaction rolls back block changes made during it: steps 1-10 again (the world may have changed),
// the script stage (12, its commandPickup may edit the world) and the pickup (13-17). Refusals are
// silent and leave the interaction alone.
//
//   auto r = blockpickup::evaluate(player, region, pos, data);   // steps 1-12, `data` filled on success
//   if (r == blockpickup::Refusal::None) blockpickup::perform(player, region, pos, std::move(data));

#include <string>
#include <string_view>

class BlockPos;
class BlockSource;
class Player;

namespace lo::feature::carry {
struct CarryData;
}

namespace lo::feature::blockpickup {

/// Why a pickup attempt was refused (first failing step of SPEC §3.1).
enum class Refusal {
    None,            ///< allowed
    Preconditions,   ///< steps 1-2: distance, game mode, hands, carrying, tick guard, carry key
    NotLoaded,       ///< chunk not loaded / nothing to capture
    Lists,           ///< step 5: black / whitelist
    DoubleHeight,    ///< step 6
    Unbreakable,     ///< step 7
    NoBlockEntity,   ///< step 8
    Locked,          ///< step 9: block actor NBT has a `Lock` key
    CustomCondition, ///< step 10
    Protection,      ///< step 11
    Script,          ///< step 12 (beforePickup hooks)
};

[[nodiscard]] std::string_view refusalName(Refusal r);

/// Steps 1-12 for the block at `pos`. On success `data` is the carry to store (type Block, state and
/// block actor SNBT, script fields written by the beforePickup hooks). Runs the protection and
/// beforePickup stages (side effects: protection event, script commandPickup) only when every
/// earlier check passed.
[[nodiscard]] Refusal evaluate(Player& player, BlockSource& region, BlockPos const& pos, carry::CarryData& data);

/// Steps 13-17: captures the block again (fresh contents), stores the carry, removes the block
/// without drops, plays the hit sound, swings, applies Slowness and logs. False when the block is
/// gone or the player already carries something (nothing changed then).
bool perform(Player& player, BlockSource& region, BlockPos const& pos, carry::CarryData data);

/// Carry On's lock test: the block actor SNBT has a `Lock` key (any value).
[[nodiscard]] bool hasLock(std::string const& blockEntitySnbt);

/// Pickups accepted in a use-block event and not yet performed (selftests).
[[nodiscard]] bool isPending(Player const& player);

} // namespace lo::feature::blockpickup
