#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "ll/api/Config.h"

#include "mod/Registry.h"

namespace lo::config {

/// Per-section JSON configs under `mods/LiftOff/config/<section>.json`.
///
/// Declare a reflectable aggregate with an integral `version` member and register it once:
///
///     struct TeleportsConfig { int version = 1; bool enableCosts = true; };
///     LO_REGISTER_CONFIG("teleports", TeleportsConfig);       // in a .cpp file
///
///     bool c = lo::config::get<TeleportsConfig>().enableCosts;    // anywhere
///
/// Files are created with defaults on first load, merged with new defaults when `version`
/// changes, and reloaded by `/carryon reload`. Configs are global (not per world).
template <class T>
T& mut() {
    static T value{};
    return value;
}

template <class T>
T const& get() {
    return mut<T>();
}

struct ConfigEntry {
    std::string                                       section;
    std::function<bool(std::filesystem::path const&)> load; ///< path of the .json file
    std::function<bool(std::filesystem::path const&)> save;
};
using ConfigRegistry = StaticRegistry<ConfigEntry>;

/// Directory holding the config files (valid after mod load).
[[nodiscard]] std::filesystem::path const& configDir();
/// Loads every registered section (creates missing files). Returns false if any failed.
bool loadAll();
/// Writes every registered section.
bool saveAll();
/// Callbacks run after `loadAll()` (e.g. to apply a changed tick interval).
void onReload(std::function<void()> fn);
/// Reloads one registered section from its file (no other file is read), then runs the reload
/// callbacks like `loadAll()`. Returns false if the section is unknown or failed to load.
/// (The client's live `carryon_client.json` reload in remote worlds.)
bool reloadSection(std::string const& section);

namespace detail {
void setConfigDir(std::filesystem::path dir);

template <ll::config::IsConfig T>
struct ConfigRegistrar {
    explicit ConfigRegistrar(std::string section) {
        ConfigRegistry::add(ConfigEntry{
            std::move(section),
            [](std::filesystem::path const& p) {
                T fresh{};
                bool ok = ll::config::loadConfig(fresh, p); // false = rewritten (missing / version change)
                if (!ok) ll::config::saveConfig(fresh, p);
                mut<T>() = std::move(fresh);
                return true;
            },
            [](std::filesystem::path const& p) { return ll::config::saveConfig(mut<T>(), p); },
        });
    }
};
} // namespace detail

} // namespace lo::config

/// Registers config struct `T` under `<configDir>/<section>.json`. `section` is a string literal.
#define LO_REGISTER_CONFIG(section, T)                                                                                \
    static ::lo::config::detail::ConfigRegistrar<T> LO_UNIQUE(lo_config_registrar_)(section)
