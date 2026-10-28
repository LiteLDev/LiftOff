#pragma once

// U7 lifecycle: death / respawn, logout / login, carried player leaving (docs/SPEC.md §3.5) and the
// `/carryon debug | clear | place` commands (§6). Server thread.
//
// Death: `ServerPlayer::die` remembers where a carrier died (world DB `lo:lifecycle:death:<uuid>`,
// so a carrier who leaves on the death screen is resolved at the next respawn). At respawn
// (`ServerPlayer::respawn`, Java's player clone) the carry is kept when the `keepInventory` game rule
// is on and the carry is not a player (re-synced one tick later; Slowness is not re-applied, like
// Carry On); otherwise it is placed at the death position with `carry::placeCarriedAt`.

#include <optional>
#include <string>
#include <vector>

#include "core/GlobalPos.h"

class Player;

namespace lo::feature::carry {
struct CarryData;
}

namespace lo::feature::lifecycle {

/// Where a carrier died (block position of the feet); nullopt when no death is pending.
[[nodiscard]] std::optional<core::GlobalPos> pendingDeath(std::string const& uuid);

/// The `keepInventory` game rule of the current level.
[[nodiscard]] bool keepInventory();

/// One `/carryon debug` line: a translated `{}` template with its argument, or (empty `key`) a
/// ready-made line from a debug-line provider.
struct DebugLine {
    std::string key;
    std::string fallback;
    std::string text;
    [[nodiscard]] std::string english() const;
    [[nodiscard]] std::string localized(Player const& reader) const;
};
[[nodiscard]] std::vector<DebugLine> debugLineParts(Player const& carrier, carry::CarryData const& data);

/// The lines `/carryon debug` prints for `data` (English when `player` is null, else the player's
/// locale): `Block: …`, `BlockState: …`, `NBT: …` / `Entity: …`, `Entity Name: …`, `NBT: …` /
/// `Carrying Player.`, then `carry::extraDebugLines`. Empty when not carrying.
[[nodiscard]] std::vector<std::string> debugLines(Player const& carrier, carry::CarryData const& data, Player const* reader);

/// `/carryon clear` for one player: deletes the carry without placing it (a carried player is
/// dismounted), removes Slowness.
void clearCarry(Player& player);

/// `/carryon place` for one player: `carry::placeCarried`, logged.
void placeCarry(Player& player);

} // namespace lo::feature::lifecycle
