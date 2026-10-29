// U7: `/carryon debug`, `/carryon clear [targets]`, `/carryon place [targets]` (docs/SPEC.md §6).

#include <cctype>
#include <string>
#include <vector>

#include "ll/api/command/CommandHandle.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/locale/I18n.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandSelector.h"
#include "mc/world/actor/player/Player.h"

#include "core/Format.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/PlaceCarried.h"
#include "feature/carry/Slowness.h"
#include "feature/lifecycle/Lifecycle.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lo::feature::lifecycle {

// Command parameter types need external linkage (reflection): no anonymous namespace.
struct CarryTargetsParams {
    CommandSelector<Player> targets;
};

namespace {
namespace carry = lo::feature::carry;

/// A message in the reader's locale (or the default locale for the console).
template <class... Args>
std::string say(Player const* reader, std::string_view key, std::string_view fallback, Args const&... args) {
    return reader ? trFor(*reader, key, fallback, args...) : tr(key, fallback, args...);
}

/// Java's entity name: the name tag, else the entity type's name.
std::string entityName(std::string const& snbt) {
    auto tag    = carry::parseNbt(snbt);
    auto string = [&](std::string_view field) -> std::string {
        auto const* t = tag ? tag->get(field) : nullptr;
        if (!t || t->getId() != Tag::Type::String) return {};
        return static_cast<std::string const&>(*static_cast<StringTag const*>(t));
    };
    if (auto custom = string("CustomName"); !custom.empty()) return custom;
    std::string id = string("identifier");
    std::string path = id.substr(id.find(':') == std::string::npos ? 0 : id.find(':') + 1);
    std::string key  = "entity." + path + ".name";
    std::string name = getI18n().get(key, nullptr);
    if (!name.empty() && name != key) return name;
    // No vanilla translations (BDS): "glow_squid" -> "Glow Squid".
    bool upper = true;
    for (auto& c : path) {
        if (c == '_') {
            c     = ' ';
            upper = true;
        } else if (upper) {
            c     = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upper = false;
        }
    }
    return path;
}

std::vector<Player*> playersOf(CarryTargetsParams const& params, CommandOrigin const& origin) {
    std::vector<Player*> out;
    for (auto* p : params.targets.results(origin)) {
        if (p) out.push_back(p);
    }
    return out;
}

/// `Cleared N Item!` / `Cleared N Items!` to the source, also logged (Java broadcasts it to ops; Bedrock
/// has no equivalent, SPEC §6).
void report(CommandOrigin const& origin, CommandOutput& output, bool placed, std::size_t n) {
    static constexpr std::string_view kKeys[2][2] = {
        {"lo.carry.cleared", "lo.carry.cleared_plural"},
        {"lo.carry.placed", "lo.carry.placed_plural"},
    };
    static constexpr std::string_view kEnglish[2][2] = {
        {"Cleared {} Item!", "Cleared {} Items!"},
        {"Placed {} Item!", "Placed {} Items!"},
    };
    auto key     = kKeys[placed][n != 1];
    auto english = kEnglish[placed][n != 1];
    log().info("{}", core::formatMessageArgs(english, n));
    output.success(say(commands::playerOf(origin), key, english, n));
}

void fail(CommandOrigin const& origin, CommandOutput& output, std::string_view key, std::string_view english) {
    output.error(say(commands::playerOf(origin), key, english));
}
void needPlayer(CommandOrigin const& o, CommandOutput& out) {
    fail(o, out, "lo.lifecycle.need_player", "This command needs a player; name the targets.");
}
void needOp(CommandOrigin const& o, CommandOutput& out) {
    fail(o, out, "lo.lifecycle.need_op", "You need operator permission.");
}
void noTargets(CommandOrigin const& o, CommandOutput& out) {
    fail(o, out, "lo.lifecycle.no_targets", "No targets matched selector");
}

void runDebug(CommandOrigin const& origin, CommandOutput& output) {
    Player* p = commands::playerOf(origin);
    if (!p) return needPlayer(origin, output);
    auto data  = carry::get(*p);
    auto lines = debugLineParts(*p, data);
    for (auto const& l : lines) {
        log().info("carry: debug {}: {}", p->getRealName(), l.english());
        output.success(l.localized(*p));
    }
}

template <bool Place>
void runSelf(CommandOrigin const& origin, CommandOutput& output) {
    Player* p = commands::playerOf(origin);
    if (Place && !commands::isAdmin(origin)) return needOp(origin, output);
    if (!p) return needPlayer(origin, output);
    if constexpr (Place) placeCarry(*p);
    else clearCarry(*p);
    report(origin, output, Place, 1);
}

template <bool Place>
void runTargets(CommandOrigin const& origin, CommandOutput& output, CarryTargetsParams const& params) {
    if (!commands::isAdmin(origin)) return needOp(origin, output);
    auto targets = playersOf(params, origin);
    if (targets.empty()) return noTargets(origin, output);
    for (auto* p : targets) {
        if constexpr (Place) placeCarry(*p);
        else clearCarry(*p);
    }
    report(origin, output, Place, targets.size());
}
} // namespace

std::string DebugLine::english() const { return key.empty() ? text : core::formatMessageArgs(fallback, text); }

std::string DebugLine::localized(Player const& reader) const {
    return key.empty() ? text : trFor(reader, key, fallback, text);
}

std::vector<DebugLine> debugLineParts(Player const& carrier, carry::CarryData const& data) {
    std::vector<DebugLine> lines;
    switch (data.type) {
    case carry::CarryType::Block:
        lines.push_back({"lo.carry.debug.block", "Block: {}", carry::carriedBlockId(data)});
        lines.push_back({"lo.carry.debug.blockstate", "BlockState: {}", data.block});
        lines.push_back({"lo.carry.debug.nbt", "NBT: {}", data.blockEntity.empty() ? std::string("{}") : data.blockEntity});
        break;
    case carry::CarryType::Entity:
        lines.push_back({"lo.carry.debug.entity", "Entity: {}", carry::entityIdOf(data.entity)});
        lines.push_back({"lo.carry.debug.entity_name", "Entity Name: {}", entityName(data.entity)});
        lines.push_back({"lo.carry.debug.nbt", "NBT: {}", data.entity});
        break;
    case carry::CarryType::Player:
        // Carry On prints only this line for a player (no model override / condition lines).
        lines.push_back({"lo.carry.debug.player", "Carrying Player.", ""});
        return lines;
    default:
        return lines;
    }
    for (auto& extra : carry::extraDebugLines(carrier, data)) lines.push_back({"", "", std::move(extra)});
    return lines;
}

std::vector<std::string> debugLines(Player const& carrier, carry::CarryData const& data, Player const* reader) {
    std::vector<std::string> out;
    for (auto const& l : debugLineParts(carrier, data)) out.push_back(reader ? l.localized(*reader) : l.english());
    return out;
}

void clearCarry(Player& player) {
    if (carry::get(player).isPlayer()) {
        // [LO] the carried player is dismounted too (the spine's eject + clear + Slowness removal).
        carry::placeCarried(player);
    } else {
        carry::clear(player);
    }
    carry::removeSlowness(player);
    log().info("carry: cleared {}", player.getRealName());
}

void placeCarry(Player& player) {
    auto r = carry::placeCarried(player);
    if (r.placed) log().info("carry: placed carried {} of {} at {} {} {} (command)", r.what, player.getRealName(), r.x, r.y, r.z);
}

} // namespace lo::feature::lifecycle

LO_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using namespace lo::feature::lifecycle;
    cmd.overload().text("debug").execute([](CommandOrigin const& o, CommandOutput& out) { runDebug(o, out); });
    cmd.overload().text("clear").execute([](CommandOrigin const& o, CommandOutput& out) { runSelf<false>(o, out); });
    cmd.overload<CarryTargetsParams>().text("clear").required("targets").execute(
        [](CommandOrigin const& o, CommandOutput& out, CarryTargetsParams const& p) { runTargets<false>(o, out, p); }
    );
    cmd.overload().text("place").execute([](CommandOrigin const& o, CommandOutput& out) { runSelf<true>(o, out); });
    cmd.overload<CarryTargetsParams>().text("place").required("targets").execute(
        [](CommandOrigin const& o, CommandOutput& out, CarryTargetsParams const& p) { runTargets<true>(o, out, p); }
    );
});
