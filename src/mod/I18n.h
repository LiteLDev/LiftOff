#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/Format.h"

class Player;

namespace lo {

/// Loads `<dir>/<locale>.json` files (flat {"key": "text"} maps merged at build time from
/// `assets/lang/<locale>/*.json`).
void loadTranslations(std::filesystem::path const& dir);

/// Looks up `key` in `locale` (empty = configured default), then en_US, then returns `fallback`.
[[nodiscard]] std::string_view translate(std::string_view key, std::string_view fallback, std::string_view locale = {});

/// The locale to use for `player` ("en_US" style), honouring `ModConfig::usePlayerLocale`.
[[nodiscard]] std::string localeOf(Player const& player);

/// Translates and formats with `{}` / `{0}` placeholders, in the default locale.
///     lo::tr("lo.menu.page", "Page {} of {}", page, pages)
template <class... Args>
[[nodiscard]] std::string tr(std::string_view key, std::string_view fallback, Args const&... args) {
    return core::formatMessageArgs(translate(key, fallback), args...);
}

/// Like `tr`, in the player's locale.
template <class... Args>
[[nodiscard]] std::string trFor(Player const& player, std::string_view key, std::string_view fallback, Args const&... args) {
    return core::formatMessageArgs(translate(key, fallback, localeOf(player)), args...);
}

} // namespace lo
