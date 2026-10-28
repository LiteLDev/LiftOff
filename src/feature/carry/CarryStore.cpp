#include "feature/carry/CarryStore.h"

#include <exception>
#include <optional>
#include <string>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/platform/UUID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::feature::carry {

namespace {
struct Entry {
    CarryData           data;
    std::optional<bool> key; ///< explicit key state (modded client / test); nullopt = sneaking
};

std::unordered_map<std::string, Entry>& cache() {
    static std::unordered_map<std::string, Entry> m;
    return m;
}

std::string keyOf(Player const& player) { return player.getUuid().asString(); }
std::string dbKey(std::string const& uuid) { return "lo:carry:" + uuid; }

Entry& entryOf(Player const& player) {
    auto  key = keyOf(player);
    auto& c   = cache();
    if (auto it = c.find(key); it != c.end()) return it->second;
    Entry e;
    if (auto j = game::db::load(dbKey(key))) {
        e.data            = CarryData::fromJson(*j);
        e.data.keyPressed = false; // the key is up after a (re)login
    }
    return c.emplace(std::move(key), std::move(e)).first->second;
}

void persist(std::string const& uuid, CarryData const& data) {
    if (data.isCarrying()) game::db::save(dbKey(uuid), data.toJson());
    else game::db::erase(dbKey(uuid));
}

void notify(Player& player, CarryData const& oldData, CarryData const& newData) {
    for (auto const& fn : ChangeListenerRegistry::entries()) {
        try {
            fn(player, oldData, newData);
        } catch (std::exception const& e) {
            log().error("carry change listener failed: {}", e.what());
        }
    }
}
} // namespace

CarryData const& get(Player const& player) { return entryOf(player).data; }

bool isCarrying(Player const& player) { return get(player).isCarrying(); }

void set(Player& player, CarryData data) {
    auto& entry     = entryOf(player);
    data.selected   = player.getSelectedItemSlot();
    data.tick       = tickCount(player);
    data.keyPressed = entry.data.keyPressed;
    CarryData old   = std::exchange(entry.data, std::move(data));
    persist(keyOf(player), entry.data);
    CarryData now = entry.data; // listeners may call set/clear again
    notify(player, old, now);
}

void clear(Player& player) {
    if (!isCarrying(player)) return;
    auto data = get(player);
    data.clear();
    set(player, std::move(data));
}

std::int64_t tickCount(Player const& player) { return player.mTickCount; }

bool canAct(Player const& player) { return tickCount(player) != get(player).tick; }

bool keyPressed(Player const& player) {
    auto& entry = entryOf(player);
    if (entry.key) return *entry.key;
    if (net::isModded(player)) return false; // modded clients report the key; nothing reported yet
    return player.isSneaking();
}

void setKeyPressed(Player& player, bool pressed) {
    auto& entry           = entryOf(player);
    entry.key             = pressed;
    entry.data.keyPressed = pressed;
}

void resetKeyPressed(Player& player) {
    auto& entry           = entryOf(player);
    entry.key             = std::nullopt;
    entry.data.keyPressed = false;
}

void onCarryChanged(ChangeListener fn) { ChangeListenerRegistry::add(std::move(fn)); }

void forEachCarrier(std::function<void(Player&, CarryData const&)> const& fn) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (auto const& d = get(p); d.isCarrying()) fn(p, d);
        return true;
    });
}

class CarryStoreModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
            // The record stays in the world DB; only the cache entry (and the key state) goes.
            cache().erase(keyOf(ev.self()));
        }));
        game::db::onWorldClosed([] { cache().clear(); });
        game::db::onWorldReady([] { cache().clear(); });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        cache().clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::feature::carry

LO_REGISTER_MODULE_ORDERED(lo::feature::carry::CarryStoreModule, lo::ModuleOrder::Services);
