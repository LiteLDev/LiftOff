#pragma once

// U6: what a carrier cannot do (docs/SPEC.md §3.4). Server side, for every client (vanilla and modded);
// the modded client additionally blocks the hotbar / container screens locally (U9).
//
//   - Hotbar lock: a slot change to another slot than `CarryData::selected` is refused
//     (`MobEquipmentPacket`) and every tick the slot is set back (`setSelectedSlot` +
//     `PlayerHotbarPacket{selected, select=true}`).
//   - Hand stays empty: inventory insertion (ground items, `/give`, `Player::add`) treats the locked slot
//     as occupied; pick block (`BlockPickRequestPacket` / `ActorPickRequestPacket`) is dropped.
//   - Use item: `PlayerUseItemEvent` cancelled.
//   - `hitWhileCarrying` false: `PlayerDestroyBlockEvent` (not the spine's synthetic protection
//     events) and `PlayerAttackEvent` cancelled, destroy progress 0.
//   - Containers: the own inventory is not opened (`ServerPlayer::openInventory`) and any other open
//     container screen is closed at the next tick (`ContainerClosePacket`, server-side container closed).
//   - `dropCarriedWhenHit`: damage > 0 that is not cancelled places the carry (`placeCarried`) at the
//     next tick, unless the carrier died (U7 handles death).
//
// Everything is wired in Restrictions.cpp (module + hooks); this header only exposes helpers that
// the selftests use.

class Player;

namespace lo::feature::restrictions {

/// Sets `player`'s selected hotbar slot to `slot` and tells the client (`PlayerHotbarPacket`).
void selectSlot(Player& player, int slot);

/// Closes the container screen `player` has open (if any): `ContainerClosePacket` to the client and
/// the server-side container manager deleted. Returns false when nothing was open.
bool closeContainer(Player& player);

} // namespace lo::feature::restrictions
