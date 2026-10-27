// Makes the mod's packs load everywhere.
//
// - Behavior pack: LeviLamina adds every mod's `behavior_packs/` to the behavior stack of every
//   world (BDS and client local worlds). Nothing to do.
// - Resource pack, client: LeviLamina adds every mod's `resource_packs/` to the client's global
//   resource stack. Nothing to do.
// - Resource pack, BDS: LeviLamina registers the pack in the repository but doesn't put it on
//   the level stack that is sent to joining players. At load (before the world opens) we add
//   it to `worlds/<level-name>/world_resource_packs.json`, and (optionally) mark it required.

#include <filesystem>
#include <fstream>
#include <string>

#include "nlohmann/json.hpp"

#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"
#include "mc/world/level/storage/LevelData.h"

#include "mod/Config.h"
#include "mod/ModConfig.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lo::pack {

namespace fs = std::filesystem;

#ifdef LO_SERVER
namespace {
std::optional<nlohmann::json> readJson(fs::path const& p) {
    std::ifstream in(p);
    if (!in) return std::nullopt;
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

std::string levelName() {
    std::ifstream in("server.properties");
    std::string   line;
    while (std::getline(in, line)) {
        if (line.starts_with("level-name=")) {
            auto v = line.substr(11);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
    }
    return "Bedrock level";
}
} // namespace
#endif

class PackDeployer {
public:
    void load() {
#ifdef LO_SERVER
        auto rpDir    = Mod::getInstance().getSelf().getModDir() / "resource_packs" / "LiftOff_RP";
        auto manifest = readJson(rpDir / "manifest.json");
        if (!manifest) {
            log().warn("resource pack manifest not found in {}", rpDir.string());
            return;
        }
        auto const& header  = (*manifest)["header"];
        auto        uuid    = header.value("uuid", std::string{});
        auto        version = header.value("version", nlohmann::json::array({0, 0, 1}));
        auto        name    = levelName();
        auto        world   = fs::path("worlds") / fs::path(std::u8string(name.begin(), name.end()));
        std::error_code ec;
        fs::create_directories(world, ec);
        auto file  = world / "world_resource_packs.json";
        auto packs = readJson(file).value_or(nlohmann::json::array());
        if (!packs.is_array()) packs = nlohmann::json::array();
        bool found = false, changed = false;
        for (auto& p : packs) {
            if (p.value("pack_id", std::string{}) != uuid) continue;
            found = true;
            if (p["version"] != version) {
                p["version"] = version;
                changed      = true;
            }
        }
        if (!found) {
            packs.push_back({
                {"pack_id", uuid   },
                {"version", version},
            });
            changed = true;
        }
        if (changed) {
            std::ofstream(file) << packs.dump(2);
            log().info("registered the resource pack in {}", file.string());
        }
#endif
    }

    void install() {
#ifdef LO_SERVER
        if (!config::get<ModConfig>().requireResourcePack) return;
        if (auto level = ll::service::getLevel()) level->getLevelData().mTexturePacksRequired = true;
#endif
    }
    void uninstall() {}
};

} // namespace lo::pack

LO_REGISTER_MODULE_ORDERED(lo::pack::PackDeployer, lo::ModuleOrder::Items);
