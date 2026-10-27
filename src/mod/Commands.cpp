// `/carryon` (alias `/liftoff`):
//   /carryon selftest [glob]     (op / console) run selftests
//   /carryon reload              (op / console) reload configs and translations
//
// Feature units add sub-commands with LO_REGISTER_SUBCOMMAND (see Commands.h).

#include "mod/Commands.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"
#include "selftest/SelfTest.h"

namespace lo::commands {

// Command parameter types need external linkage (reflection), so no anonymous namespace here.
struct SelftestParams {
    std::string glob;
};

Player* playerOf(CommandOrigin const& origin) {
    auto* e = origin.getEntity();
    return (e && e->isPlayer()) ? static_cast<Player*>(e) : nullptr;
}

bool isAdmin(CommandOrigin const& origin) {
    return origin.getPermissionsLevel() >= CommandPermissionLevel::GameDirectors;
}

namespace {

void registerAll() {
    if (!ll::service::getCommandRegistry()) return;
    auto& cmd = ll::command::CommandRegistrar::getServerInstance().getOrCreateCommand(
        "carryon",
        "LiftOff (Carry On)",
        CommandPermissionLevel::Any
    );
    cmd.alias("liftoff");

    auto selftest = [](CommandOrigin const& origin, CommandOutput& output, std::string const& glob) {
        if (!isAdmin(origin)) return output.error("You need operator permission.");
        std::int64_t key = 0;
        if (auto* p = playerOf(origin)) key = p->getOrCreateUniqueID().rawID;
        selftest::runAll(glob, [key](std::string const& line) {
            if (!key) return; // console: the log already prints every line
            auto level = ll::service::getLevel();
            if (!level) return;
            ActorUniqueID id;
            id.rawID = key;
            if (auto* p = level->getPlayer(id)) p->sendMessage(line);
        });
        output.success("SELFTEST started");
    };
    cmd.overload().text("selftest").execute([selftest](CommandOrigin const& origin, CommandOutput& output) {
        selftest(origin, output, "*");
    });
    cmd.overload<SelftestParams>().text("selftest").required("glob").execute(
        [selftest](CommandOrigin const& origin, CommandOutput& output, SelftestParams const& params) {
            selftest(origin, output, params.glob);
        }
    );

    cmd.overload().text("reload").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!isAdmin(origin)) return output.error("You need operator permission.");
        bool ok = config::loadAll();
        loadTranslations(Mod::getInstance().getSelf().getModDir() / "lang");
        if (ok) output.success("LiftOff configuration reloaded.");
        else output.error("Some config files failed to load; see the log.");
    });

    for (auto const& fn : SubcommandRegistry::entries()) fn(cmd);
}
} // namespace

class CommandModule {
public:
    void install() {
        registerAll(); // BDS: the registry already exists when mods are enabled
        mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::command::ServerCommandRegisterEvent>(
            [](auto&) { registerAll(); } // client: every local world
        );
    }
    void uninstall() { ll::event::EventBus::getInstance().removeListener(mListener); }

private:
    ll::event::ListenerPtr mListener;
};

} // namespace lo::commands

LO_REGISTER_MODULE_ORDERED(lo::commands::CommandModule, lo::ModuleOrder::Commands);
