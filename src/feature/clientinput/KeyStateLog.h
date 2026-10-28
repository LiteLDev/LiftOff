#pragma once

// U9 clientinput, server side (both builds): logs every change of a modded player's carry key state
// as it arrived from the client (`net::KeyPressedPacket` -> `carry::keyPressed`), one line per change:
//     clientinput: <player> carry key down|up
// so e2e scenarios (local worlds and remote servers) can wait for the key to arrive. Server thread.
//
// Also hosts the e2e fixture selftest `clientinput.e2e_fixture`: a real (non-simulated) player tagged
// `lo_e2e_carry` gets a carry (the block under its feet, nothing removed), one tagged `lo_e2e_uncarry`
// loses it, one tagged `lo_e2e_check` must carry and still be on the locked hotbar slot; the tags are
// removed. Without tagged players it does nothing.

#include <optional>

class Player;

namespace lo::feature::clientinput {

inline constexpr char const* kFixtureCarryTag   = "lo_e2e_carry";
inline constexpr char const* kFixtureUncarryTag = "lo_e2e_uncarry";
inline constexpr char const* kFixtureCheckTag   = "lo_e2e_check";

/// The key state last logged for a modded player (nullopt: none seen since it joined).
[[nodiscard]] std::optional<bool> loggedKeyState(Player const& player);

} // namespace lo::feature::clientinput
