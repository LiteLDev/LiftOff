#include "mod/I18n.h"

#include <fstream>
#include <map>
#include <unordered_map>

#include "nlohmann/json.hpp"

#include "mc/world/actor/player/Player.h"

#include "mod/Config.h"
#include "mod/ModConfig.h"
#include "mod/Log.h"

namespace lo {

namespace {
using Table = std::unordered_map<std::string, std::string>;
std::map<std::string, Table, std::less<>> gTables;

std::string normalize(std::string_view locale) {
    std::string s(locale);
    for (auto& c : s)
        if (c == '-') c = '_';
    return s;
}

Table const* find(std::string_view locale) {
    if (auto it = gTables.find(locale); it != gTables.end()) return &it->second;
    return nullptr;
}
} // namespace

void loadTranslations(std::filesystem::path const& dir) {
    gTables.clear();
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path());
        auto          j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) {
            log().warn("lang file {} is not a JSON object", entry.path().string());
            continue;
        }
        auto& table = gTables[normalize(entry.path().stem().string())];
        for (auto& [k, v] : j.items()) {
            if (v.is_string()) table[k] = v.get<std::string>();
        }
    }
    log().debug("loaded {} languages", gTables.size());
}

std::string_view translate(std::string_view key, std::string_view fallback, std::string_view locale) {
    std::string const candidates[] = {normalize(locale), normalize(config::get<ModConfig>().language), "en_US"};
    for (auto const& c : candidates) {
        if (c.empty()) continue;
        if (auto* t = find(c)) {
            if (auto it = t->find(std::string(key)); it != t->end()) return it->second;
        }
    }
    return fallback;
}

std::string localeOf(Player const& player) {
    auto const& cfg = config::get<ModConfig>();
    if (!cfg.usePlayerLocale || player.isSimulated()) return cfg.language;
    auto code = normalize(player.getLocaleCode());
    if (code.empty() || !find(code)) return cfg.language;
    return code;
}

} // namespace lo
