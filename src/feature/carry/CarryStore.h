#pragma once

// Per-player carry state (server thread). The single source of truth for "what does this player
// carry"; every unit reads and writes it through these functions.
//
//   if (carry::isCarrying(player)) ...
//   auto data = carry::get(player);          // copy, edit, write back:
//   data.type = CarryType::Block; ...
//   carry::set(player, std::move(data));     // refreshes selected slot + tick, persists, syncs
//   carry::clear(player);
//
// Persistence: the record lives in the world DB under `lo:carry:<player uuid>` (written on every
// change, erased when cleared), so a carrier keeps carrying across logout/login and server restarts
// (Carry On keeps it in the player data). Modded clients get every change through
// `net::CarrySyncPacket` automatically.
//
// Never keep the returned reference across ticks or across calls to `set` / `clear`.

#include <cstdint>
#include <functional>

#include "feature/carry/CarryData.h"
#include "mod/Registry.h"

class Player;

namespace lo::feature::carry {

/// The player's carry (type Invalid when not carrying). Loaded from the world DB on first use.
[[nodiscard]] CarryData const& get(Player const& player);
[[nodiscard]] bool             isCarrying(Player const& player);

/// Stores `data`; refreshes `data.selected` (current hotbar slot) and `data.tick` (current tick
/// count) like Carry On's `setCarryData`, persists it and notifies the change listeners.
void set(Player& player, CarryData data);
/// Clears the carried object (keeps keyPressed). Does not place anything or remove Slowness
/// (see PlaceCarried.h / Slowness.h). No-op when not carrying.
void clear(Player& player);

/// The player's tick count (Java `player.tickCount`; Bedrock `Actor::mTickCount`).
[[nodiscard]] std::int64_t tickCount(Player const& player);
/// Tick guard: false in the tick the carry last changed (no pickup + place in one click).
[[nodiscard]] bool canAct(Player const& player);

/// The carry key state: the value last sent by a modded client (KeyPressedPacket) or set with
/// `setKeyPressed`; vanilla clients: `isSneaking()`.
[[nodiscard]] bool keyPressed(Player const& player);
/// Sets the key state (network handler; selftests). Also updates `CarryData::keyPressed` without
/// changing the tick or notifying listeners.
void setKeyPressed(Player& player, bool pressed);
/// Back to "sneaking = key" (selftests).
void resetKeyPressed(Player& player);

/// Called after every change of a player's carry (set / clear), on the server thread.
using ChangeListener = std::function<void(Player& player, CarryData const& oldData, CarryData const& newData)>;
using ChangeListenerRegistry = StaticRegistry<ChangeListener>;
/// Adds a listener (at static init or in `install()`; never removed).
void onCarryChanged(ChangeListener fn);

/// Calls `fn` for every online player that carries something.
void forEachCarrier(std::function<void(Player& player, CarryData const& data)> const& fn);

} // namespace lo::feature::carry
