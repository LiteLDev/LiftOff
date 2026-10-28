#pragma once

// U9 clientinput (client build only, docs/SPEC.md §4.6, §3.4): the remappable "Carry" key and the
// client-side restrictions while the local player carries something.
//
// - Key: `key.carry.desc` ("Carry"), default Shift, registered with LeviLamina's `KeyRegistry` so it
//   is listed (and remappable) in Settings > Controls > Keyboard & Mouse.
// - Every client tick the sent key state is computed (`sentKeyState`) and, when it changed and the
//   server runs LiftOff, sent with `client::carry::sendKeyPressed`.
// - While `client::carry::localData()` says the local player carries: hotbar slot changes are
//   blocked (and the slot is snapped back every client tick), container screens (inventory,
//   creative inventory, chests, ...) are closed right after they open, pick block is cancelled.
//
// Client thread unless noted.

#ifdef LO_CLIENT

namespace lo::client::input {

/// The key's name in LeviLamina's `KeyRegistry` (registered without a mod prefix, so its
/// controls-screen action is Carry On's `key.carry.desc`).
inline constexpr char const* kCarryKeyName = "carry.desc";
/// The controls-screen action / translation key of the key.
inline constexpr char const* kCarryKeyAction = "key.carry.desc";
/// Default key code: Shift (Bedrock keyboard codes are Windows virtual-key codes; VK_SHIFT, the
/// game's sneak key, which Carry On's default "Left Shift" maps to).
inline constexpr int kDefaultCarryKey = 16;

/// What the sent key state is made of.
struct KeyInputs {
    bool keyBound;          ///< the Carry key has a key assigned
    bool keyDown;           ///< the Carry key is held (button down/up events)
    bool mouseAndKeyboard;  ///< the active input mode is mouse & keyboard (not gamepad / touch)
    bool noScreen;          ///< in the world with no menu screen open (Java: keys are released while a screen is open)
    bool sneaking;          ///< the local player is sneaking
};

/// The carry key state sent to the server: the Carry key when it is bound and the input is mouse &
/// keyboard (Java `KeyMapping.isDown()`: false while a screen is open); otherwise the sneak state
/// (Carry On's fallback for an unbound key; [LO] also for gamepad / touch).
[[nodiscard]] constexpr bool sentKeyState(KeyInputs const& in) {
    if (!in.keyBound || !in.mouseAndKeyboard) return in.sneaking;
    return in.keyDown && in.noScreen;
}

/// The hotbar lock: `lockedSlot` < 0 = not locked. A slot change of `inventory` to `slot` is blocked
/// when `inventory` is the locked (local player's) inventory and `slot` is not the locked slot.
struct HotbarLock {
    void const* inventory  = nullptr;
    int         lockedSlot = -1;
};
[[nodiscard]] constexpr bool blocksSlotChange(HotbarLock const& lock, void const* inventory, int slot) {
    return lock.lockedSlot >= 0 && inventory != nullptr && inventory == lock.inventory && slot != lock.lockedSlot;
}

/// The Carry key is held (raw key state, any thread).
[[nodiscard]] bool carryKeyDown();
/// The Carry key has a key assigned in the controls (true until the game reported its mappings).
[[nodiscard]] bool carryKeyBound();
/// The key state last sent to the server (false when nothing was sent in this level yet).
[[nodiscard]] bool lastSentKeyState();
/// The local player carries something (as last synced): the restrictions are active (any thread).
[[nodiscard]] bool restrictionsActive();

} // namespace lo::client::input

#endif // LO_CLIENT
