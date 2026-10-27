#include "game/WorldDb.h"

#include <exception>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/db_helpers/Category.h"

#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::game::db {

namespace {
constexpr int kAutosaveTicks = 20 * 30;

LevelStorage* levelStorage() {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    return &level->getLevelStorage();
}

std::vector<std::function<void()>>& readyListeners() {
    static std::vector<std::function<void()>> v;
    return v;
}
std::vector<std::function<void()>>& saveListeners() {
    static std::vector<std::function<void()>> v;
    return v;
}
std::vector<std::function<void()>>& closedListeners() {
    static std::vector<std::function<void()>> v;
    return v;
}

void fire(std::vector<std::function<void()>> const& list, char const* what) {
    for (auto const& fn : list) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("world {} listener failed: {}", what, e.what());
        }
    }
}
} // namespace

bool available() { return levelStorage() != nullptr; }

std::optional<nlohmann::json> load(std::string const& key) {
    auto* db = levelStorage();
    if (!db) return std::nullopt;
    std::string buffer;
    if (!db->loadData(key, buffer, DBHelpers::Category::Uncategorized)) return std::nullopt;
    auto j = nlohmann::json::parse(buffer, nullptr, false);
    if (j.is_discarded()) {
        log().error("world data {} is unreadable (kept untouched)", key);
        return std::nullopt;
    }
    return j;
}

void save(std::string const& key, nlohmann::json const& value) {
    auto* db = levelStorage();
    if (!db) return;
    db->saveData(key, value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), DBHelpers::Category::Uncategorized);
}

void erase(std::string const& key) {
    if (auto* db = levelStorage()) db->deleteData(key, DBHelpers::Category::Uncategorized);
}

void forEachKey(std::string const& prefix, std::function<void(std::string_view)> const& fn) {
    auto* db = levelStorage();
    if (!db) return;
    db->forEachKeyWithPrefix(prefix, DBHelpers::Category::Uncategorized, [&](std::string_view key, std::string_view) {
        if (key.starts_with(prefix)) key.remove_prefix(prefix.size()); // some DB versions pass the full key
        fn(key);
    });
}

void onWorldReady(std::function<void()> fn) { readyListeners().push_back(std::move(fn)); }
void onWorldSave(std::function<void()> fn) { saveListeners().push_back(std::move(fn)); }
void onWorldClosed(std::function<void()> fn) { closedListeners().push_back(std::move(fn)); }

class WorldDbModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        // Client: every local world load. (BDS: already started when mods are enabled.)
        mListeners.push_back(bus.emplaceListener<ll::event::ServerStartedEvent>([](auto&) { ready(); }));
        if (available()) ready();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerStoppingEvent>([](auto&) { close(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            if (++mTicks < kAutosaveTicks) return;
            mTicks = 0;
            fire(saveListeners(), "save");
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (available()) close();
    }

private:
    static void ready() {
        fire(readyListeners(), "ready");
        log().info("LiftOff world ready");
    }
    static void close() {
        fire(saveListeners(), "save");
        fire(closedListeners(), "closed");
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    int                                 mTicks = 0;
};

} // namespace lo::game::db

LO_REGISTER_MODULE_ORDERED(lo::game::db::WorldDbModule, lo::ModuleOrder::Storage);
