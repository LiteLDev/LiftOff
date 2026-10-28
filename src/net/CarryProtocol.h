#pragma once

// LiftOff client <-> server protocol (Carry On's packets, docs/SPEC.md §1, §3.3, §4).
//
// Packets (LeviLamina custom packets, one JSON payload each):
//   HelloPacket            c->s  {protocol}            modded client joined a level (retried until welcomed)
//   WelcomePacket          s->c  {protocol}            the server runs LiftOff; followed by the carries of the level
//   KeyPressedPacket       c->s  {pressed}             carry key state changed (Carry On `key_pressed`)
//   CarrySyncPacket        s->c  {carrier, runtime, data}  a player's carry (Carry On `sync_carry_data`)
//   StartRidingPacket      s->c  {entity, ride}        the client mounts / dismounts `entity` on itself
//   StartRidingOtherPacket s->c  {mount, rider, ride}  `rider` mounts / dismounts `mount`
//
// The server only ever sends to players that said hello (`isModded`), so vanilla clients on BDS never
// see a custom packet. Carry syncs are automatic: every `CarryStore` change is sent to every modded
// player of the level, and a hello is answered with the carries of all online carriers. Ids are
// `ActorUniqueID::rawID`.
//
// Client build: the mirror below holds what the server sent (client thread).

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

class Actor;
class Player;

namespace lo::feature::carry {
struct CarryData;
}

namespace lo::net {

inline constexpr int kProtocolVersion = 1;

/// The player runs the LiftOff client mod (said hello with a compatible protocol).
[[nodiscard]] bool isModded(Player const& player);
/// Called on the server thread after a modded player said hello (after the welcome + syncs).
void onModdedJoined(std::function<void(Player&)> fn);

/// Sends `carrier`'s current carry to every modded player of the level (automatic on every change).
void sendCarrySync(Player& carrier);
/// Sends `carrier`'s carry to one modded player.
void sendCarrySyncTo(Player& receiver, Player& carrier);
/// Carry On `start_riding`: tells `to` (if modded) to mount / dismount `entity` on itself.
void sendStartRiding(Player& to, Actor const& entity, bool ride);
/// Carry On `start_riding_other`: tells every modded player of the level that `rider` mounts /
/// dismounts `mount`.
void broadcastStartRidingOther(Actor const& mount, Actor const& rider, bool ride);

// ---- selftest seams ---------------------------------------------------------------------------

/// Marks a player (e.g. a SimulatedPlayer) as modded without a hello; packets "sent" to it are
/// recorded instead (`takeTestMessages`).
void setTestModded(Player const& player, bool modded);
/// The recorded packets of a test-modded player: (packet name, payload), oldest first; cleared on read.
[[nodiscard]] std::vector<std::pair<std::string, nlohmann::json>> takeTestMessages(Player const& player);
/// Handles a client packet as if `player` had sent it ("HelloPacket", "KeyPressedPacket").
void injectFromClient(Player& player, std::string const& packet, nlohmann::json const& payload);

} // namespace lo::net

#ifdef LO_CLIENT
namespace lo::client::carry {

/// The carry of the player with unique id `uniqueId` as last synced (nullptr: not carrying / unknown).
[[nodiscard]] feature::carry::CarryData const* carriedOf(std::int64_t uniqueId);
/// The local player's carry (nullptr: not carrying / not in a level).
[[nodiscard]] feature::carry::CarryData const* localData();
/// The server accepted our hello (it runs LiftOff).
[[nodiscard]] bool serverHasMod();
/// Sends the carry key state (no-op unless `serverHasMod()`).
void sendKeyPressed(bool pressed);

/// Client-thread listeners (register in `load()` / `install()`).
void onSync(std::function<void(std::int64_t uniqueId, feature::carry::CarryData const& data)> fn);
void onStartRiding(std::function<void(std::int64_t entity, bool ride)> fn);
void onStartRidingOther(std::function<void(std::int64_t mount, std::int64_t rider, bool ride)> fn);
/// After the server accepted our hello.
void onConnected(std::function<void()> fn);

} // namespace lo::client::carry
#endif
