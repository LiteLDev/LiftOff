// Selftests of the scripts unit (U8, dx 800..899): script object matching, every condition, priority,
// override_checks through the Hooks stages, commandPickup / commandLoop / commandPlace, custom pickup
// conditions and the debug line. Each test sets `useScripts` and its scripts itself and restores them.

#include <string>
#include <utility>
#include <vector>

#include "fmt/format.h"

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/scripts/Scripts.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry   = lo::feature::carry;
namespace scripts = lo::feature::scripts;
using carry::CarryData;
using carry::CarryType;
using nlohmann::json;

/// Sets `useScripts` and the loaded scripts for one test; restores both afterwards.
class ScriptEnv {
public:
    explicit ScriptEnv(std::vector<json> const& defs, bool use = true) {
        auto& s     = lo::config::mut<carry::CarryConfig>().settings;
        mUseScripts = s.useScripts;
        s.useScripts = use;
        std::vector<scripts::Script> list;
        int                          n = 0;
        for (auto const& d : defs) {
            std::string id = d.value("_id", "lo_test/" + std::to_string(n++));
            auto        s2 = lo::core::scripts::parseScript(d, id);
            if (s2) list.push_back(std::move(*s2));
        }
        mCount = list.size();
        mOld   = scripts::exchangeScripts(std::move(list));
    }
    ~ScriptEnv() {
        lo::config::mut<carry::CarryConfig>().settings.useScripts = mUseScripts;
        (void)scripts::exchangeScripts(std::move(mOld));
    }
    ScriptEnv(ScriptEnv const&)            = delete;
    ScriptEnv& operator=(ScriptEnv const&) = delete;
    [[nodiscard]] std::size_t count() const { return mCount; }

private:
    bool                         mUseScripts = false;
    std::size_t                  mCount      = 0;
    std::vector<scripts::Script> mOld;
};

/// Sets the custom pickup conditions for one test; restores them afterwards.
class ConditionEnv {
public:
    ConditionEnv(std::vector<std::string> blocks, std::vector<std::string> entities) {
        auto& c = lo::config::mut<carry::CarryConfig>().customPickupConditions;
        mOld    = c;
        c.customPickupConditionsBlocks   = std::move(blocks);
        c.customPickupConditionsEntities = std::move(entities);
        scripts::rebuildConditions();
    }
    ~ConditionEnv() {
        lo::config::mut<carry::CarryConfig>().customPickupConditions = mOld;
        scripts::rebuildConditions();
    }
    ConditionEnv(ConditionEnv const&)            = delete;
    ConditionEnv& operator=(ConditionEnv const&) = delete;

private:
    carry::CarryConfig::CustomPickupConditions mOld;
};

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

Actor* spawnMob(Context& ctx, char const* id, lo::core::GlobalPos pos) {
    auto level = ll::service::getLevel();
    Vec3 at    = lo::game::blockPos(pos).bottomCenter();
    for (int i = 0; i < 10; ++i) { // spawned mobs are sometimes babies (smaller): keep an adult
        auto* mob =
            level->getSpawner().spawnMob(regionOf(ctx), ActorDefinitionIdentifier(id), nullptr, at, false, false, false);
        if (!mob || !mob->isBaby()) return mob;
        mob->remove();
    }
    return nullptr;
}

json blockScript(json object, json rest = json::object()) {
    rest["object"] = {{"block", std::move(object)}};
    if (!rest.contains("render") && !rest.contains("conditions") && !rest.contains("effects")
        && !rest.contains("override_checks")) {
        rest["render"] = json::object();
    }
    return rest;
}

json entityScript(json object, json rest = json::object()) {
    rest["object"] = {{"entity", std::move(object)}};
    if (!rest.contains("render") && !rest.contains("conditions") && !rest.contains("effects")
        && !rest.contains("override_checks")) {
        rest["render"] = json::object();
    }
    return rest;
}

std::string idOf(scripts::Script const* s) { return s ? s->id : "<none>"; }
} // namespace

LO_REGISTER_SELFTEST("scripts.disabled", [](Context& ctx) {
    auto&     p = ctx.player();
    ScriptEnv env({blockScript({{"name", "minecraft:stone"}}, {{"override_checks", true}, {"_id", "a"}})}, false);
    ctx.require(env.count() == 1, "script parsed");
    auto const*         stone = lo::game::blockWith("minecraft:stone");
    BlockPos            pos{0, 0, 0};
    std::string         none;
    carry::PickupTarget t{p, regionOf(ctx), stone, &pos, &none};
    ctx.check(scripts::matchBlock(*stone, "") != nullptr, "the object matches");
    ctx.check(scripts::matchTarget(t) == nullptr, "no script without useScripts");
    ctx.check(!carry::overrideChecks(t), "no override without useScripts");
    CarryData d;
    ctx.check(carry::beforePickup(t, d) && d.activeScript.empty(), "no script activated without useScripts");
});

LO_REGISTER_SELFTEST("scripts.object_block", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(800);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:chest"), "chest placed");
    co_await ctx.ticks(1);
    auto&    region = regionOf(ctx);
    BlockPos at     = lo::game::blockPos(pos);
    auto     box    = region.tryGetContainer(at);
    ctx.require(box.has_value(), "the chest has a container");
    box->setItem(3, lo::game::makeItem("minecraft:diamond", 5));
    auto cap = carry::captureBlock(region, at);
    ctx.require(cap && cap->hasBlockEntity(), "chest captured");
    auto const& chest = region.getBlock(at);
    auto const* stone = lo::game::blockWith("minecraft:stone");

    ScriptEnv env({
        blockScript({{"name", "minecraft:chest"}, {"nbt", {{"Items", {{{"Name", "minecraft:stick"}}}}}}}, {{"_id", "stick"}}),
        blockScript({{"name", "minecraft:chest"}, {"hardness", ">5"}}, {{"_id", "hard"}}),
        blockScript({{"name", "minecraft:chest"}, {"resistance", "<0"}}, {{"_id", "resist"}}),
        blockScript({{"name", "minecraft:*chest"}, {"hardness", "<=5"}, {"nbt", {{"Items", {{{"Name", "minecraft:diamond"}, {"Count", 5}}}}}}}, {{"_id", "diamond"}}),
        blockScript({{"name", "minecraft:stone"}}, {{"_id", "stone"}}),
        blockScript({{"name", "minecraft:stone"}, {"nbt", {{"x", 1}}}}, {{"_id", "stone_nbt"}}),
    });
    ctx.require(env.count() == 6, "6 scripts parsed");
    ctx.check(idOf(scripts::matchBlock(chest, cap->blockEntity)) == "diamond", "name + hardness + partial NBT match the chest");
    ctx.check(idOf(scripts::matchBlock(*stone, "")) == "stone", "name match without NBT");
    ctx.check(idOf(scripts::matchBlock(*lo::game::blockWith("minecraft:dirt"), "")) == "<none>", "no script for dirt");
    box->removeAllItems();
});

LO_REGISTER_SELFTEST("scripts.object_entity", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(810);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto* pig = spawnMob(ctx, "minecraft:pig", pos);
    ctx.require(pig != nullptr, "a pig spawned");
    pig->setNameTag("lo_script_pig");
    co_await ctx.ticks(2);
    ScriptEnv env({
        entityScript({{"name", "minecraft:cow"}}, {{"_id", "cow"}}),
        entityScript({{"name", "minecraft:pig"}, {"health", ">100"}}, {{"_id", "healthy"}}),
        entityScript({{"name", "minecraft:pig"}, {"height", ">5"}}, {{"_id", "tall"}}),
        entityScript({{"name", "minecraft:pig"}, {"width", "<0.5"}}, {{"_id", "thin"}}),
        entityScript({{"name", "minecraft:pig"}, {"nbt", {{"CustomName", "other"}}}}, {{"_id", "other"}}),
        entityScript({{"name", "minecraft:p*"}, {"health", ">0"}, {"height", "<2"}, {"width", "<=1"}, {"nbt", {{"CustomName", "lo_script_pig"}}}}, {{"_id", "pig"}}),
    });
    ctx.require(env.count() == 6, "6 scripts parsed");
    auto matched = idOf(scripts::matchEntity(*pig));
    ctx.check(matched == "pig", fmt::format("name + health + size + partial NBT match the pig (matched {})", matched));
    pig->remove();
});

LO_REGISTER_SELFTEST("scripts.priority", [](Context& ctx) {
    auto const* stone = lo::game::blockWith("minecraft:stone");
    {
        ScriptEnv env({
            blockScript({{"name", "minecraft:stone"}}, {{"_id", "low"}}),
            blockScript({{"name", "minecraft:stone"}}, {{"_id", "high"}, {"priority", 10}}),
            blockScript({{"name", "minecraft:stone"}}, {{"_id", "neg"}, {"priority", -3}}),
        });
        ctx.check(idOf(scripts::matchBlock(*stone, "")) == "high", "the highest priority wins");
        ctx.check(scripts::scripts().front().id == "high" && scripts::scripts().back().id == "neg", "sorted highest first");
    }
    {
        ScriptEnv env({
            blockScript({{"name", "minecraft:stone"}}, {{"_id", "first"}}),
            blockScript({{"name", "minecraft:stone"}}, {{"_id", "second"}}),
        });
        ctx.check(idOf(scripts::matchBlock(*stone, "")) == "first", "equal priority keeps the load order");
    }
});

LO_REGISTER_SELFTEST("scripts.conditions", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    auto cond = [&](json c) {
        auto s = lo::core::scripts::parseScript(entityScript(json::object(), {{"conditions", std::move(c)}}), "c");
        return s && scripts::conditionsMet(*s, p);
    };
    ctx.check(cond(json::object()), "no condition: met");
    ctx.check(cond({{"advancement", "minecraft:story/mine_stone"}}), "advancement always passes");

    // gamestage -> tag carryon:stage:<stage>
    ctx.check(!cond({{"gamestage", "lo_iron"}}), "gamestage missing");
    p.addTag("carryon:stage:lo_iron");
    ctx.check(cond({{"gamestage", "lo_iron"}}), "gamestage via tag");
    p.removeTag("carryon:stage:lo_iron");

    // xp level
    ctx.check(cond({{"xp", "<1"}}), "xp level 0");
    p.addLevels(5);
    ctx.check(cond({{"xp", ">=5"}}) && !cond({{"xp", ">5"}}), "xp level 5");
    p.addLevels(-5);

    // gamemode (Java ids)
    ctx.check(cond({{"gamemode", "0"}}) && !cond({{"gamemode", "1"}}), "survival is 0");
    scripts::runAsPlayer(p, "gamemode creative @s");
    ctx.check(cond({{"gamemode", "1"}}) && cond({{"gamemode", ">=1"}}), "creative is 1");
    scripts::runAsPlayer(p, "gamemode spectator @s");
    ctx.check(cond({{"gamemode", "3"}}), "spectator is 3");
    scripts::runAsPlayer(p, "gamemode survival @s");

    // scoreboard
    scripts::runAsPlayer(p, "scoreboard objectives add lo_script_s dummy");
    ctx.check(!cond({{"scoreboard", "lo_script_s>=0"}}), "no score: not met");
    scripts::runAsPlayer(p, "scoreboard players set @s lo_script_s 4");
    ctx.check(scripts::playerScore(p, "lo_script_s") == 4, "score set");
    ctx.check(cond({{"scoreboard", "lo_script_s>=3"}}) && cond({{"scoreboard", "lo_script_s=4"}}), "score >= 3 / = 4");
    ctx.check(!cond({{"scoreboard", "lo_script_s<4"}}), "score < 4 fails");
    ctx.check(!cond({{"scoreboard", "lo_missing>=0"}}), "unknown objective fails");
    scripts::runAsPlayer(p, "scoreboard objectives remove lo_script_s");

    // position (block coordinates; an axis with base 0 is not checked)
    auto bp = p.getFeetBlockPos();
    auto at = [&](int ox, int d) {
        return "x=" + std::to_string(bp.x + ox) + ",y=" + std::to_string(bp.y) + ",z=" + std::to_string(bp.z)
             + ",dx=" + std::to_string(d) + ",dy=0,dz=0";
    };
    if (bp.x != 0 && bp.x - 1 != 0 && bp.y != 0 && bp.z != 0) {
        ctx.check(cond({{"position", at(0, 0)}}), "position at the player");
        ctx.check(cond({{"position", at(-1, 2)}}), "position inside the range");
        ctx.check(!cond({{"position", at(1, 3)}}), "position outside the range");
    }
    ctx.check(cond({{"position", "x=0,y=0,z=0"}}), "base-0 axes are not checked");

    // effects
    ctx.check(!cond({{"effects", "minecraft:speed#0"}}), "missing effect");
    scripts::runAsPlayer(p, "effect @s speed 60 1 true");
    ctx.check(cond({{"effects", "minecraft:speed#1"}}) && cond({{"effects", "speed"}}), "speed II satisfies #1 and #0");
    ctx.check(!cond({{"effects", "speed#2"}}), "speed II does not satisfy #2");
    ctx.check(!cond({{"effects", "speed,haste"}}), "every effect is required");
    ctx.check(!cond({{"effects", "luck"}}), "effects missing on Bedrock never match");
    scripts::runAsPlayer(p, "effect @s clear");

    // all conditions together
    p.addTag("carryon:stage:lo_all");
    ctx.check(cond({{"gamestage", "lo_all"}, {"gamemode", "0"}, {"xp", "0"}}), "all conditions met");
    ctx.check(!cond({{"gamestage", "lo_all"}, {"gamemode", "1"}}), "one failing condition fails the script");
    p.removeTag("carryon:stage:lo_all");
});

LO_REGISTER_SELFTEST("scripts.override_checks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(820);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto* stand = spawnMob(ctx, "minecraft:armor_stand", pos);
    auto* pig   = spawnMob(ctx, "minecraft:pig", ctx.testPos(822));
    ctx.require(stand && pig, "an armor stand and a pig spawned");
    co_await ctx.ticks(1);
    ctx.check(!carry::isEntityAllowed(*stand), "armor stands are blacklisted");

    auto const* stone = lo::game::blockWith("minecraft:stone");
    auto const* dirt  = lo::game::blockWith("minecraft:dirt");
    BlockPos    bp    = lo::game::blockPos(pos);
    std::string none;
    carry::PickupTarget stoneT{p, regionOf(ctx), stone, &bp, &none};
    carry::PickupTarget dirtT{p, regionOf(ctx), dirt, &bp, &none};
    carry::PickupTarget standT{p, regionOf(ctx)};
    standT.entity = stand;
    carry::PickupTarget pigT{p, regionOf(ctx)};
    pigT.entity = pig;
    {
        ScriptEnv env({
            blockScript({{"name", "minecraft:stone"}}, {{"override_checks", true}}),
            blockScript({{"name", "minecraft:dirt"}}, {{"override_checks", false}, {"render", json::object()}}),
            entityScript({{"name", "minecraft:armor_stand"}}, {{"override_checks", true}}),
        });
        ctx.require(env.count() == 3, "3 scripts parsed");
        ctx.check(carry::overrideChecks(stoneT), "a block without block entity passes with override_checks");
        ctx.check(!carry::overrideChecks(dirtT), "override_checks false does not override");
        ctx.check(carry::overrideChecks(standT), "a blacklisted entity passes with override_checks");
        ctx.check(!carry::overrideChecks(pigT), "no matching script: no override");
    }
    {
        ScriptEnv none({});
        ctx.check(!carry::overrideChecks(stoneT), "no override without scripts");
    }
    stand->remove();
    pig->remove();
});

LO_REGISTER_SELFTEST("scripts.pickup_command", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    auto const* barrel = lo::game::blockWith("minecraft:barrel");
    BlockPos    bp{0, 0, 0};
    std::string none;
    carry::PickupTarget t{p, regionOf(ctx), barrel, &bp, &none};
    json render = {{"name_block", "minecraft:stone"}, {"scale", "x=2,y=2,z=2"}};
    ScriptEnv env({
        blockScript(
            {{"name", "minecraft:barrel"}},
            {{"_id", "lo/barrel"},
             {"conditions", {{"gamestage", "lo_cmd"}}},
             {"render", render},
             {"effects", {{"commandPickup", "tag @s add lo_script_picked"}}}}
        ),
    });
    ctx.require(env.count() == 1, "script parsed");

    CarryData d;
    ctx.check(!carry::beforePickup(t, d), "conditions not met: pickup refused");
    ctx.check(d.activeScript.empty(), "no script activated");
    ctx.check(!p.hasTag("lo_script_picked"), "commandPickup not run");

    p.addTag("carryon:stage:lo_cmd");
    ctx.check(carry::beforePickup(t, d), "conditions met: pickup allowed");
    ctx.check(d.activeScript == "lo/barrel", "the script is active");
    ctx.check(d.scriptRender == render, "the render object is stored for the clients");
    ctx.check(p.hasTag("lo_script_picked"), "commandPickup ran as the player");
    p.removeTag("lo_script_picked");
    p.removeTag("carryon:stage:lo_cmd");
});

LO_REGISTER_SELFTEST("scripts.loop_and_place", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    ScriptEnv env({
        blockScript(
            {{"name", "minecraft:barrel"}},
            {{"_id", "lo/loop"},
             {"effects",
              {{"commandLoop", "scoreboard players add @s lo_script_loop 1"},
               {"commandPlace", "tag @s add lo_script_placed"}}}}
        ),
    });
    ctx.require(env.count() == 1, "script parsed");
    scripts::runAsPlayer(p, "scoreboard objectives add lo_script_loop dummy");
    scripts::runAsPlayer(p, "scoreboard players set @s lo_script_loop 0");

    CarryData d;
    d.type         = CarryType::Block;
    d.block        = carry::toSnbt(*lo::game::blockWith("minecraft:barrel")->mSerializationId);
    d.activeScript = "lo/loop";
    carry::set(p, d);
    co_await ctx.ticks(10);
    auto during = scripts::playerScore(p, "lo_script_loop").value_or(-1);
    ctx.check(during >= 8 && during <= 12, "commandLoop ran every tick (" + std::to_string(during) + " in 10 ticks)");

    carry::runOnPlace(p, carry::get(p));
    ctx.check(p.hasTag("lo_script_placed"), "commandPlace ran");
    carry::clear(p);
    co_await ctx.ticks(1);
    auto after = scripts::playerScore(p, "lo_script_loop").value_or(-1);
    co_await ctx.ticks(5);
    ctx.check(scripts::playerScore(p, "lo_script_loop").value_or(-1) == after, "commandLoop stops with the carry");

    // A carry without an active script runs nothing.
    d.activeScript.clear();
    carry::set(p, d);
    co_await ctx.ticks(3);
    ctx.check(scripts::playerScore(p, "lo_script_loop").value_or(-1) == after, "no loop without an active script");
    carry::clear(p);
    p.removeTag("lo_script_placed");
    scripts::runAsPlayer(p, "scoreboard objectives remove lo_script_loop");
});

LO_REGISTER_SELFTEST("scripts.custom_conditions", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(830);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto* pig = spawnMob(ctx, "minecraft:pig", pos);
    auto* cow = spawnMob(ctx, "minecraft:cow", ctx.testPos(832));
    ctx.require(pig && cow, "a pig and a cow spawned");
    co_await ctx.ticks(1);

    ConditionEnv env(
        {"minecraft:barrel[facing_direction=1](lo_up)", "minecraft:barrel(lo_any)", "  ", "bad(", "minecraft:chest"},
        {"minecraft:pig(lo_pig)", "minecraft:*(lo_mob)"}
    );
    auto const* barrelUp = lo::game::blockWith("minecraft:barrel", {{"facing_direction", 1}});
    auto const* barrelN  = lo::game::blockWith("minecraft:barrel", {{"facing_direction", 2}});
    auto const* stone    = lo::game::blockWith("minecraft:stone");
    ctx.require(barrelUp && barrelN && stone, "blocks exist");
    BlockPos    bp = lo::game::blockPos(pos);
    std::string none;
    auto        blockT = [&](Block const* b) { return carry::PickupTarget{p, regionOf(ctx), b, &bp, &none}; };
    auto        entityT = [&](Actor* a) {
        carry::PickupTarget t{p, regionOf(ctx)};
        t.entity = a;
        return t;
    };

    ctx.check(carry::customConditions(blockT(stone)), "no matching entry: met");
    ctx.check(!carry::customConditions(blockT(barrelUp)), "matching entry without the stage: refused");
    p.addTag("carryon:stage:lo_any");
    ctx.check(carry::customConditions(blockT(barrelN)), "the state filter skips the first entry");
    ctx.check(!carry::customConditions(blockT(barrelUp)), "the first matching entry decides");
    p.addTag("carryon:stage:lo_up");
    ctx.check(carry::customConditions(blockT(barrelUp)), "stage present: met");

    ctx.check(!carry::customConditions(entityT(pig)), "pig without stage");
    p.addTag("carryon:stage:lo_mob");
    ctx.check(!carry::customConditions(entityT(pig)), "pig needs its own (first) entry");
    ctx.check(carry::customConditions(entityT(cow)), "cow matches the wildcard entry");
    p.addTag("carryon:stage:lo_pig");
    ctx.check(carry::customConditions(entityT(pig)), "pig with its stage");

    // /carryon debug line (U7 prints it).
    CarryData d;
    d.type     = CarryType::Block;
    d.block    = carry::toSnbt(*barrelUp->mSerializationId);
    auto lines = carry::extraDebugLines(p, d);
    ctx.check(
        lines.size() == 1 && lines[0].find("minecraft:barrel[facing_direction=1](lo_up)") != std::string::npos,
        "debug line names the matching block entry"
    );
    d.type   = CarryType::Entity;
    d.block  = {};
    d.entity = carry::captureActor(*cow);
    lines    = carry::extraDebugLines(p, d);
    ctx.check(lines.size() == 1 && lines[0].find("minecraft:*(lo_mob)") != std::string::npos, "debug line for entities");
    d.type   = CarryType::Player;
    d.entity = {};
    ctx.check(carry::extraDebugLines(p, d).empty(), "no line for a carried player");

    for (auto const* tag : {"lo_any", "lo_up", "lo_mob", "lo_pig"}) p.removeTag(std::string("carryon:stage:") + tag);
    pig->remove();
    cow->remove();
});

LO_REGISTER_SELFTEST("scripts.files", [](Context& ctx) {
    // The e2e scenario `scripts` seeds config/scripts/lo_e2e/*.json; without them there is nothing to check.
    auto const* s = scripts::findScript("lo_e2e/pickup");
    if (!s) {
        ctx.info("no seeded scripts (run the e2e scenario 'scripts')");
        return;
    }
    ctx.check(lo::config::get<carry::CarryConfig>().settings.useScripts, "useScripts is on in carryon.json");
    ctx.check(s->isBlock && s->priority == 5, "lo_e2e/pickup: block script with priority 5");
    ctx.check(s->commandPickup == "tag @s add lo_e2e_picked", "lo_e2e/pickup: commandPickup");
    ctx.check(scripts::findScript("lo_e2e/sub/entity") != nullptr, "scripts in sub folders are loaded");
    ctx.check(scripts::findScript("lo_e2e/invalid") == nullptr, "invalid scripts are skipped");
    ctx.check(scripts::scripts().front().id == "lo_e2e/pickup", "highest priority first");
});
