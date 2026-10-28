#include "feature/scripts/Scripts.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/ScoreInfo.h"
#include "mc/world/scores/Scoreboard.h"
#include "mc/world/scores/ScoreboardId.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::scripts {

namespace cs = core::scripts;
using nlohmann::json;

namespace {
std::vector<Script>& loaded() {
    static std::vector<Script> v;
    return v;
}

struct Conditions {
    std::vector<CustomCondition> blocks, entities;
};
Conditions& conditions() {
    static Conditions c;
    return c;
}

bool useScripts() { return config::get<carry::CarryConfig>().settings.useScripts; }

/// Partial NBT compare of `expected` against SNBT text; no block actor ("") matches (Carry On).
bool snbtMatches(json const& expected, std::string_view snbt, bool emptyMatches) {
    if (snbt.empty()) return emptyMatches;
    auto actual = cs::parseSnbt(snbt);
    return actual && cs::nbtMatches(expected, *actual);
}

int xpLevel(Player const& player) {
    auto ref = player.getAttribute(Player::LEVEL());
    auto ptr = ref.mPtr;
    return ptr ? static_cast<int>(ptr->mCurrentValue) : 0;
}

} // namespace

std::optional<int> playerScore(Player const& player, std::string const& objective) {
    auto level = ll::service::getLevel();
    if (!level) return std::nullopt;
    auto& board = level->getScoreboard();
    auto* obj   = board.getObjective(objective);
    if (!obj) return std::nullopt;
    auto const& id = board.getScoreboardId(player);
    if (id == ScoreboardId::INVALID()) return std::nullopt;
    auto info = obj->getPlayerScore(id);
    if (!info.mValid) return std::nullopt;
    return static_cast<int>(info.mValue);
}

namespace {

bool effectsMet(Player const& player, std::string_view text) {
    auto reqs = cs::parseEffects(text);
    if (!reqs) return false;
    for (auto const& r : *reqs) {
        if (r.id < 0) return false;
        auto const* inst = player.getEffect(static_cast<uint>(r.id));
        if (!inst || inst->mAmplifier < r.amplifier) return false;
    }
    return true;
}
} // namespace

// ---- scripts ---------------------------------------------------------------------------------

void reloadScripts() {
    namespace fs = std::filesystem;
    auto                  dir = config::configDir() / "scripts";
    std::error_code       ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc; // a bad entry must not end the scan
        if (it->is_regular_file(fileEc) && it->path().extension() == ".json") files.push_back(it->path());
    }
    if (ec) log().warn("scripts: could not list {}: {}", dir.string(), ec.message());
    std::sort(files.begin(), files.end());
    std::vector<Script> out;
    int                 invalid = 0;
    for (auto const& file : files) {
        auto rel = file.lexically_relative(dir);
        rel.replace_extension();
        auto          u8 = rel.generic_u8string();
        auto          id = std::string(u8.begin(), u8.end());
        std::ifstream in(file, std::ios::binary);
        auto          j = json::parse(in, nullptr, false, true);
        std::string   error;
        auto          s = j.is_discarded() ? std::nullopt : cs::parseScript(j, id, &error);
        if (j.is_discarded()) error = "not valid JSON";
        if (!s) {
            ++invalid;
            log().warn("scripts: {} skipped: {}", id, error);
            continue;
        }
        out.push_back(std::move(*s));
    }
    cs::sortScripts(out);
    loaded() = std::move(out);
    log().info("scripts: loaded {} script(s), {} invalid", loaded().size(), invalid);
}

std::vector<Script> const& scripts() { return loaded(); }

std::vector<Script> exchangeScripts(std::vector<Script> next) {
    cs::sortScripts(next);
    std::swap(loaded(), next);
    return next;
}

Script const* findScript(std::string_view id) {
    for (auto const& s : loaded()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

Script const* matchBlock(Block const& block, std::string_view blockEntitySnbt) {
    auto const& type = block.getBlockType();
    for (auto const& s : loaded()) {
        if (!s.isBlock) continue;
        if (s.name && !s.name->matches(carry::blockCandidate(block))) continue;
        if (!cs::matchesBound(s.hardness, type.getDestroySpeed())) continue;
        if (!cs::matchesBound(s.resistance, type.getExplosionResistance())) continue;
        if (s.nbt && !snbtMatches(*s.nbt, blockEntitySnbt, true)) continue;
        return &s;
    }
    return nullptr;
}

Script const* matchEntity(Actor& entity) {
    std::optional<std::string> snbt; // captured on first use
    for (auto const& s : loaded()) {
        if (s.isBlock) continue;
        if (s.name && !s.name->matchesId(carry::entityCandidate(entity))) continue;
        if (!cs::matchesBound(s.health, entity.getHealth())) continue;
        if (!cs::matchesBound(s.height, carry::entityHeight(entity))) continue;
        if (!cs::matchesBound(s.width, carry::entityWidth(entity))) continue;
        if (s.nbt) {
            if (!snbt) snbt = carry::captureActor(entity);
            if (!snbtMatches(*s.nbt, *snbt, false)) continue;
        }
        return &s;
    }
    return nullptr;
}

Script const* matchTarget(carry::PickupTarget const& target) {
    if (!useScripts()) return nullptr;
    if (target.isBlock()) {
        return matchBlock(*target.block, target.blockEntity ? std::string_view(*target.blockEntity) : std::string_view{});
    }
    if (target.entity) return matchEntity(*target.entity);
    return nullptr;
}

bool conditionsMet(Script const& s, Player& player) {
    if (!s.gamestage.empty() && !hasStage(player, s.gamestage)) return false;
    // advancement: always passes (no advancements on Bedrock, SPEC §8).
    if (!cs::matchesBound(s.xp, xpLevel(player))) return false;
    if (!s.gamemode.empty()) {
        int mode = cs::javaGameModeId(static_cast<int>(carry::gameType(player)));
        if (!cs::matchesBound(s.gamemode, mode)) return false;
    }
    if (!s.scoreboard.empty()) {
        auto cond = cs::parseScore(s.scoreboard);
        if (!cond) return false;
        auto value = playerScore(player, cond->objective);
        if (!value || !cond->bound.test(*value)) return false;
    }
    if (!s.position.empty()) {
        auto cond = cs::parsePosition(s.position);
        if (!cond) return false;
        auto p = player.getFeetBlockPos();
        if (!cs::positionMatches(*cond, p.x, p.y, p.z)) return false;
    }
    if (!s.effects.empty() && !effectsMet(player, s.effects)) return false;
    return true;
}

bool runAsPlayer(Player& player, std::string_view command) {
    while (!command.empty() && (command.front() == ' ' || command.front() == '/')) command.remove_prefix(1);
    if (command.empty()) return true;
    auto level = ll::service::getLevel();
    if (!level) return false;
    std::string name;
    for (char c : player.getRealName()) {
        if (c == '"' || c == '\\') name.push_back('\\');
        name.push_back(c);
    }
    std::string full = "execute as \"" + name + "\" run " + std::string(command);
    // Carry On runs the command from the server source (full permissions, server position).
    ServerCommandOrigin origin("LiftOff", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
    auto out = ll::command::CommandRegistrar::getServerInstance().executeCommand(full, origin);
    return out.mSuccessCount > 0;
}

// ---- custom pickup conditions -----------------------------------------------------------------

void rebuildConditions() {
    auto const& cfg = config::get<carry::CarryConfig>().customPickupConditions;
    Conditions  c;
    auto        compile = [](std::vector<std::string> const& in, std::vector<CustomCondition>& out, char const* name) {
        for (auto const& text : in) {
            if (auto cond = cs::parseCustomCondition(text)) {
                out.push_back(std::move(*cond));
            } else {
                log().warn("carryon.json {}: invalid entry '{}' ignored", name, text);
            }
        }
    };
    compile(cfg.customPickupConditionsBlocks, c.blocks, "customPickupConditionsBlocks");
    compile(cfg.customPickupConditionsEntities, c.entities, "customPickupConditionsEntities");
    conditions() = std::move(c);
}

CustomCondition const* blockCondition(Block const& block) {
    auto candidate = carry::blockCandidate(block);
    for (auto const& c : conditions().blocks) {
        if (c.entry.matches(candidate)) return &c;
    }
    return nullptr;
}

CustomCondition const* entityCondition(Actor const& entity) {
    auto candidate = carry::entityCandidate(entity);
    for (auto const& c : conditions().entities) {
        if (c.entry.matchesId(candidate)) return &c;
    }
    return nullptr;
}

CustomCondition const* entityConditionById(std::string_view id) {
    core::carry::Candidate candidate{id, {}, {}};
    for (auto const& c : conditions().entities) {
        if (c.entry.matchesId(candidate)) return &c;
    }
    return nullptr;
}

bool hasStage(Player const& player, std::string_view stage) { return player.hasTag(cs::stageTag(stage)); }

bool customConditionMet(carry::PickupTarget const& target) {
    CustomCondition const* c = nullptr;
    if (target.isBlock()) c = blockCondition(*target.block);
    else if (target.entity) c = entityCondition(*target.entity);
    return !c || hasStage(target.player, c->stage);
}

// ---- hooks -----------------------------------------------------------------------------------

namespace {
/// Runs a script command; the first failure of each (script, command) is logged (no per-tick logging).
void runScriptCommand(Player& player, Script const& s, std::string const& command, char const* kind) {
    if (command.empty() || runAsPlayer(player, command)) return;
    static std::vector<std::string> reported;
    auto key = s.id + "/" + kind + "/" + command;
    if (std::find(reported.begin(), reported.end(), key) != reported.end()) return;
    reported.push_back(std::move(key));
    log().warn("scripts: {} {} '{}' failed for {} (later failures are not logged)", s.id, kind, command, player.getRealName());
}

void runEffect(Player& player, carry::CarryData const& data, std::string Script::* command, char const* kind) {
    if (data.activeScript.empty()) return;
    if (auto const* s = findScript(data.activeScript)) runScriptCommand(player, *s, s->*command, kind);
}
} // namespace

// Lowest priority: other hooks may still veto in beforePickup before commandPickup runs.
LO_REGISTER_PICKUP_HOOKS(carry::PickupHooks{
    .name           = "scripts",
    .priority       = -1000,
    .overrideChecks = [](carry::PickupTarget const& t) {
        auto const* s = matchTarget(t);
        return s && s->overrideChecks;
    },
    .customCondition = [](carry::PickupTarget const& t) { return customConditionMet(t); },
    .beforePickup    = [](carry::PickupTarget const& t, carry::CarryData& data) {
        auto const* s = matchTarget(t);
        if (!s) return true;
        if (!conditionsMet(*s, t.player)) return false;
        data.activeScript = s->id;
        data.scriptRender = s->render;
        runScriptCommand(t.player, *s, s->commandPickup, "commandPickup");
        return true;
    },
    .onPlace = [](Player& p, carry::CarryData const& data) { runEffect(p, data, &Script::commandPlace, "commandPlace"); },
    .onLoop  = [](Player& p, carry::CarryData const& data) { runEffect(p, data, &Script::commandLoop, "commandLoop"); },
});

LO_REGISTER_DEBUG_LINES([](Player const& player, carry::CarryData const& data, std::vector<std::string>& lines) {
    CustomCondition const* c = nullptr;
    if (data.isBlock()) {
        if (auto const* block = carry::carriedBlock(data)) c = blockCondition(*block);
    } else if (data.isEntity()) {
        c = entityConditionById(carry::entityIdOf(data.entity));
    }
    if (c) lines.push_back(trFor(player, "lo.carry.debug.condition", "Custom Pickup Condition: {}", c->text));
});

/// Loads the scripts and the custom pickup conditions; reloads them on /carryon reload.
class ScriptsModule {
public:
    void install() {
        rebuildConditions();
        reloadScripts();
        if (!mHooked) {
            config::onReload([] {
                rebuildConditions();
                reloadScripts();
            });
            mHooked = true;
        }
    }
    void uninstall() {}

private:
    bool mHooked = false;
};

} // namespace lo::feature::scripts

LO_REGISTER_MODULE(lo::feature::scripts::ScriptsModule);
