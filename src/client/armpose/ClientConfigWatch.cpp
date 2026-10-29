// Live `carryon_client.json` on modded clients in dedicated-server worlds (docs/SPEC.md §5.2): Carry On
// applies its client settings at once from its in-game config screen; LiftOff has no screen, so the
// client reloads the file when it changes. There `/carryon reload` runs on the server and reloads the
// server's files only. In a local world the integrated server shares the config objects, so the file is
// reloaded by `/carryon reload` there (as every other file) and not behind the server thread's back.
//
// About once a second (ClientLevelTickEvent) the file's modification time is compared with the last
// one seen; a changed file that stayed unchanged for one more check and parses as JSON is reloaded
// (`config::reloadSection`), which runs the reload callbacks: renderArms is sent to the server again
// (net), model overrides are recompiled (U12); facePlayer / rotateEntitiesSideways are read per frame.

#ifdef LO_CLIENT

#include <filesystem>
#include <fstream>
#include <optional>

#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::client::armpose {

namespace {
namespace fs = std::filesystem;

constexpr int  kCheckInterval = 20; ///< ticks
constexpr char kSection[]     = "carryon_client";

fs::path configFile() { return config::configDir() / (std::string(kSection) + ".json"); }

std::optional<fs::file_time_type> writeTime() {
    std::error_code ec;
    auto            t = fs::last_write_time(configFile(), ec);
    if (ec) return std::nullopt;
    return t;
}

bool parses(fs::path const& p) {
    std::ifstream in(p);
    if (!in) return false;
    return !nlohmann::json::parse(in, nullptr, false, true).is_discarded();
}

struct Watch {
    std::optional<fs::file_time_type> loaded;  ///< time of the file last loaded
    std::optional<fs::file_time_type> pending; ///< changed time seen at the last check
    int                               ticks = 0;
};
Watch& watch() {
    static Watch w;
    return w;
}

void check() {
    auto& w = watch();
    auto  t = writeTime();
    if (!t || t == w.loaded) {
        w.pending.reset();
        return;
    }
    // A local world: the integrated server reads the same objects; `/carryon reload` reloads there.
    if (ll::service::getLevel()) {
        w.pending.reset();
        return;
    }
    if (w.pending != t) { // still being written? wait one more check
        w.pending = t;
        return;
    }
    w.pending.reset();
    if (!parses(configFile())) {
        log().warn("client config: {}.json is not valid JSON; keeping the current settings", kSection);
        w.loaded = t; // retried on the next change
        return;
    }
    bool ok  = config::reloadSection(kSection);
    w.loaded = writeTime(); // the load may rewrite the file (new defaults)
    if (ok) log().info("client config: reloaded {}.json", kSection);
    else log().warn("client config: could not reload {}.json", kSection);
}
} // namespace

class ClientConfigWatchModule {
public:
    void install() {
        watch().loaded = writeTime(); // loaded at mod load
        mListener      = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            if (++watch().ticks % kCheckInterval == 0) check();
        });
    }
    void uninstall() {
        if (mListener) ll::event::EventBus::getInstance().removeListener(mListener);
        mListener = nullptr;
    }

private:
    ll::event::ListenerPtr mListener;
};

} // namespace lo::client::armpose

LO_REGISTER_MODULE(lo::client::armpose::ClientConfigWatchModule);

#endif
