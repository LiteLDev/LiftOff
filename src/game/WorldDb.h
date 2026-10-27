#pragma once

// Per-world JSON storage in the world's own LevelDB (server thread only).
//
// Keys are "lo:<unit>:<...>" strings. Values are JSON documents. Data travels with the world
// folder (BDS worlds/, client local worlds). The world lifecycle hooks let modules reset their
// caches: on the client every local world load starts a new world in the same process.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace lo::game::db {

/// A level (and its DB) is loaded.
[[nodiscard]] bool available();

[[nodiscard]] std::optional<nlohmann::json> load(std::string const& key);
void                                        save(std::string const& key, nlohmann::json const& value);
void                                        erase(std::string const& key);
/// Calls `fn(keySuffix)` for every stored key starting with `prefix`.
void forEachKey(std::string const& prefix, std::function<void(std::string_view suffix)> const& fn);

/// World lifecycle (server thread). Listeners registered at static init or in `install()`.
/// - `onWorldReady`: a world is loaded (BDS: at mod enable; client: each local world load).
/// - `onWorldSave`: autosave (every 30 s), server stopping and mod disable: write dirty data now.
/// - `onWorldClosed`: after the final save of a world: drop caches.
void onWorldReady(std::function<void()> fn);
void onWorldSave(std::function<void()> fn);
void onWorldClosed(std::function<void()> fn);

} // namespace lo::game::db
