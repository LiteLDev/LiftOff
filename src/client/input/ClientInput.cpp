// U9 clientinput: the Carry key and the client restrictions while carrying (see ClientInput.h).

#ifdef LO_CLIENT

#include "client/input/ClientInput.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/input/KeyRegistry.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/input/KeyboardRemappingLayout.h"
#include "mc/client/input/Keymapping.h"
#include "mc/client/input/VanillaClientInputMappingFactory.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/player/PlayerInventory.h"

#include "client/Client.h"
#include "feature/carry/CarryData.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace lo::client::input {

namespace {
using Clock = std::chrono::steady_clock;

// Raw key state (button handlers, client thread) and whether the key is bound (controls update).
std::atomic<bool> gKeyDown{false};
std::atomic<bool> gKeyBound{true};
// The state last sent to the server in this level (client thread).
bool gSent = false;
// The module is installed (the sync listener cannot be removed; it does nothing while uninstalled).
std::atomic<bool> gInstalled{false};

// The hotbar lock, read by the PlayerInventory hook on any thread (the integrated server's players
// have their own inventories, never equal to the local player's).
std::atomic<void const*> gLockInventory{nullptr};
std::atomic<int>         gLockSlot{-1};

// Container screen close requests (client thread): one request per screen (controller + screen name),
// forgotten once that screen is no longer on top, retried if it is still ticking after kCloseRetry.
void const*       gClosing = nullptr;
std::string       gClosingName;
Clock::time_point gClosingAt{};
constexpr auto    kCloseRetry = std::chrono::milliseconds(500);
// Rate limit for the "blocked" log lines (one per user action, not one per repeated call).
Clock::time_point gLastSlotLog{};
constexpr auto    kLogInterval = std::chrono::milliseconds(1000);

HotbarLock currentLock() { return {gLockInventory.load(), gLockSlot.load()}; }

void const* inventoryOf(LocalPlayer& player) { return player.mInventory.get(); }

/// Updates the hotbar lock from the carry mirror (client thread).
void refreshLock() {
    if (!gInstalled) return;
    auto* player = currentPlayer();
    auto* data   = player ? carry::localData() : nullptr;
    if (!player || !data || !data->isCarrying()) {
        if (gLockSlot.exchange(-1) >= 0) log().info("clientinput: carry ended; hotbar unlocked");
        gLockInventory = nullptr;
        return;
    }
    gLockInventory = inventoryOf(*player);
    int slot       = data->selected;
    if (gLockSlot.exchange(slot) != slot) log().info("clientinput: carrying; hotbar locked to slot {}", slot);
}

void resetLevelState() {
    gKeyDown       = false;
    gSent          = false;
    gLockSlot      = -1;
    gLockInventory = nullptr;
    gClosing       = nullptr;
    gClosingName.clear();
}

/// One client tick: hotbar snap-back and the key state.
void clientTick() {
    auto* client = currentClient();
    auto* player = currentPlayer();
    if (!client || !player) return;

    refreshLock();
    if (int slot = gLockSlot.load(); slot >= 0 && player->getSelectedItemSlot() != slot) {
        player->setSelectedSlot(slot);
    }

    bool noScreen = client->isInWorldAndNotShowingAnyMenuScreens();
    // Like Java's KeyMapping.releaseAll() when a screen opens: a release while a screen has the input
    // must not leave the key stuck down.
    if (!noScreen) gKeyDown = false;
    if (gClosing && client->getTopScreenName() != gClosingName) {
        gClosing = nullptr;
        gClosingName.clear();
    }

    bool state = sentKeyState({
        .keyBound         = gKeyBound.load(),
        .keyDown          = gKeyDown.load(),
        .mouseAndKeyboard = client->currentInputModeIsMouseAndKeyboard(),
        .noScreen         = noScreen,
        .sneaking         = player->isSneaking(),
    });
    if (state == gSent || !carry::serverHasMod()) return;
    gSent = state;
    carry::sendKeyPressed(state);
    log().info("clientinput: carry key {} (sent)", state ? "down" : "up");
}

/// Closes the container screen `controller` belongs to (it is the top screen while it ticks).
void closeContainerScreen(void const* controller) {
    auto* client = currentClient();
    if (!client) return;
    auto now = Clock::now();
    if (controller == gClosing && now - gClosingAt < kCloseRetry) return;
    auto name    = client->getTopScreenName();
    gClosing     = controller;
    gClosingName = name;
    gClosingAt   = now;
    client->getCurrentSceneStack()->schedulePopScreenWithExpectedNames({name});
    log().info("clientinput: closed {} while carrying", name);
}
} // namespace

bool carryKeyDown() { return gKeyDown.load(); }
bool carryKeyBound() { return gKeyBound.load(); }
bool lastSentKeyState() { return gSent; }
bool restrictionsActive() { return gLockSlot.load() >= 0; }

// ---- hooks -------------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    SelectSlotHook,
    ll::memory::HookPriority::Normal,
    PlayerInventory,
    &PlayerInventory::selectSlot,
    bool,
    int         slot,
    ContainerID containerId
) {
    if (blocksSlotChange(currentLock(), this, slot)) {
        auto now = Clock::now();
        if (now - gLastSlotLog >= kLogInterval) {
            gLastSlotLog = now;
            log().info("clientinput: hotbar slot change to {} blocked while carrying", slot);
        }
        return false;
    }
    return origin(slot, containerId);
}

LL_TYPE_INSTANCE_HOOK(
    PickBlockHook,
    ll::memory::HookPriority::Normal,
    LocalPlayer,
    &LocalPlayer::pickBlock,
    void,
    ::HitResult const& hitResult,
    bool               withData
) {
    if (restrictionsActive()) {
        log().info("clientinput: pick block cancelled while carrying");
        return;
    }
    origin(hitResult, withData);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerTickHook,
    ll::memory::HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$tick,
    ::ui::DirtyFlag
) {
    auto result = origin();
    if (restrictionsActive()) closeContainerScreen(this);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    KeyboardControlsHook,
    ll::memory::HookPriority::Normal,
    VanillaClientInputMappingFactory,
    &VanillaClientInputMappingFactory::$_updateKeyboardAndMouseControls,
    void,
    ::IOptionRegistry& options
) {
    origin(options);
    if (auto layout = mKeyboardRemappingLayout.lock()) {
        bool bound = layout->getKeymappingByAction(kCarryKeyAction).isAssigned();
        if (gKeyBound.exchange(bound) != bound) {
            log().info("clientinput: carry key {}", bound ? "bound" : "unbound (the sneak state is sent instead)");
        }
    }
}

// ---- module ------------------------------------------------------------------------------------

class ClientInputModule {
public:
    // The key must exist before the game builds its default key mappings (game start).
    void load() {
        auto& key = ll::input::KeyRegistry::getInstance().getOrCreateKey(
            kCarryKeyName,
            {kDefaultCarryKey},
            true,
            std::weak_ptr<ll::mod::Mod>{}
        );
        key.registerButtonDownHandler([](FocusImpact, IClientInstance&) { gKeyDown = true; });
        key.registerButtonUpHandler([](FocusImpact, IClientInstance&) { gKeyDown = false; });
    }

    void install() {
        gInstalled = true;
        SelectSlotHook::hook();
        PickBlockHook::hook();
        ContainerTickHook::hook();
        KeyboardControlsHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { clientTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) { resetLevelState(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { resetLevelState(); }));
        if (!mSyncHooked) {
            mSyncHooked = true;
            // Lock / unlock as soon as the carry arrives, not one tick later.
            carry::onSync([](std::int64_t, feature::carry::CarryData const&) { refreshLock(); });
            carry::onConnected([] { gSent = false; });
        }
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        KeyboardControlsHook::unhook();
        ContainerTickHook::unhook();
        PickBlockHook::unhook();
        SelectSlotHook::unhook();
        gInstalled = false;
        resetLevelState();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
    bool                                mSyncHooked = false;
};

} // namespace lo::client::input

LO_REGISTER_MODULE(lo::client::input::ClientInputModule);

// ---- selftests (client build; the logic of the hooks, the key registration and the strings) -----

namespace {
using lo::selftest::Context;
namespace in = lo::client::input;

LO_REGISTER_SELFTEST("clientinput.key", [](Context& ctx) {
    auto& reg = ll::input::KeyRegistry::getInstance();
    ctx.require(reg.hasKey(in::kCarryKeyName), "the Carry key is registered");
    auto keys = reg.getRegisteredKeys();
    ctx.check(
        std::find(keys.begin(), keys.end(), std::string(in::kCarryKeyName)) != keys.end(),
        "registered without a mod prefix (controls action key.carry.desc)"
    );
    auto& key = reg.getOrCreateKey(in::kCarryKeyName, {in::kDefaultCarryKey}, true, std::weak_ptr<ll::mod::Mod>{});
    ctx.check(key.isValid(), "the key handle is valid");
    ctx.check(key.isAllowRemap(), "the key is remappable");
    ctx.check(key.getKeyCodes() == std::vector<int>{16}, "default key: Shift (16)");
    ctx.check(key.getFullName() == "carry.desc", "full name carry.desc");
});

LO_REGISTER_SELFTEST("clientinput.key_state", [](Context& ctx) {
    using K = in::KeyInputs;
    // Bound key, mouse & keyboard: the key, released while a screen is open; sneaking is ignored.
    ctx.check(in::sentKeyState(K{true, true, true, true, false}), "held key, no screen: down");
    ctx.check(!in::sentKeyState(K{true, false, true, true, true}), "sneaking without the key: up");
    ctx.check(!in::sentKeyState(K{true, true, true, false, false}), "held key while a screen is open: up");
    // Unbound key: the sneak state (Carry On), whatever the key does.
    ctx.check(in::sentKeyState(K{false, false, true, true, true}), "unbound: sneaking is down");
    ctx.check(!in::sentKeyState(K{false, true, true, true, false}), "unbound: not sneaking is up");
    // Gamepad / touch [LO]: the sneak state.
    ctx.check(in::sentKeyState(K{true, false, false, true, true}), "gamepad: sneaking is down");
    ctx.check(!in::sentKeyState(K{true, true, false, true, false}), "gamepad: not sneaking is up");
});

LO_REGISTER_SELFTEST("clientinput.hotbar_lock", [](Context& ctx) {
    int  a = 0, b = 0;
    auto locked = in::HotbarLock{&a, 3};
    ctx.check(in::blocksSlotChange(locked, &a, 5), "a different slot of the locked inventory is blocked");
    ctx.check(!in::blocksSlotChange(locked, &a, 3), "the locked slot itself is allowed (snap-back, server)");
    ctx.check(!in::blocksSlotChange(locked, &b, 5), "other inventories (integrated server players) are free");
    ctx.check(!in::blocksSlotChange(in::HotbarLock{&a, -1}, &a, 5), "not carrying: free");
    ctx.check(!in::blocksSlotChange(in::HotbarLock{nullptr, 3}, nullptr, 5), "no local inventory: free");
});

LO_REGISTER_SELFTEST("clientinput.strings", [](Context& ctx) {
    ctx.check(lo::translate("key.carry.desc", "?", "en_US") == "Carry", "key.carry.desc (en_US)");
    ctx.check(lo::translate("key.carry.desc", "?", "zh_CN") == "搬运", "key.carry.desc (zh_CN)");
    ctx.check(
        lo::translate("key.category.carryon.key.carry.category", "?", "en_US") == "Carry On",
        "key.category.carryon.key.carry.category (en_US)"
    );
    ctx.info(std::string("client: key ") + (in::carryKeyBound() ? "bound" : "unbound") + ", " +
             (in::carryKeyDown() ? "down" : "up") + ", restrictions " + (in::restrictionsActive() ? "on" : "off"));
});
} // namespace

#endif // LO_CLIENT
