#include "mod/Config.h"

#include <exception>

#include "mod/Log.h"

namespace lo::config {

namespace {
std::filesystem::path              gDir;
std::vector<std::function<void()>> gReloadCallbacks;
} // namespace

namespace detail {
void setConfigDir(std::filesystem::path dir) { gDir = std::move(dir); }
} // namespace detail

std::filesystem::path const& configDir() { return gDir; }

bool loadAll() {
    std::error_code ec;
    std::filesystem::create_directories(gDir, ec);
    bool ok = true;
    for (auto const& e : ConfigRegistry::entries()) {
        auto path = gDir / (e.section + ".json");
        try {
            if (!e.load(path)) ok = false;
        } catch (std::exception const& ex) {
            ok = false;
            log().error("config {}: failed to load {}: {}", e.section, path.string(), ex.what());
        }
    }
    for (auto const& cb : gReloadCallbacks) cb();
    return ok;
}

bool saveAll() {
    bool ok = true;
    for (auto const& e : ConfigRegistry::entries()) {
        try {
            if (!e.save(gDir / (e.section + ".json"))) ok = false;
        } catch (std::exception const& ex) {
            ok = false;
            log().error("config {}: failed to save: {}", e.section, ex.what());
        }
    }
    return ok;
}

void onReload(std::function<void()> fn) { gReloadCallbacks.push_back(std::move(fn)); }

bool reloadSection(std::string const& section) {
    for (auto const& e : ConfigRegistry::entries()) {
        if (e.section != section) continue;
        auto path = gDir / (e.section + ".json");
        try {
            if (!e.load(path)) return false;
        } catch (std::exception const& ex) {
            log().error("config {}: failed to load {}: {}", e.section, path.string(), ex.what());
            return false;
        }
        for (auto const& cb : gReloadCallbacks) cb();
        return true;
    }
    return false;
}

} // namespace lo::config
