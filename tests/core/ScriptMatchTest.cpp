#include <gtest/gtest.h>

#include "core/ScriptMatch.h"

using namespace lo::core::scripts;
using nlohmann::json;

TEST(ScriptMatch, NumberBounds) {
    EXPECT_TRUE(matchesBound("", 3));
    EXPECT_TRUE(matchesBound("  ", 3));
    EXPECT_TRUE(matchesBound("<=5", 5));
    EXPECT_FALSE(matchesBound("<=5", 5.5));
    EXPECT_TRUE(matchesBound(">=5", 5));
    EXPECT_FALSE(matchesBound(">=5", 4.9));
    EXPECT_TRUE(matchesBound("<5", 4.9));
    EXPECT_FALSE(matchesBound("<5", 5));
    EXPECT_TRUE(matchesBound(">5", 5.1));
    EXPECT_FALSE(matchesBound(">5", 5));
    EXPECT_TRUE(matchesBound("=2", 2));
    EXPECT_TRUE(matchesBound("2", 2));
    EXPECT_FALSE(matchesBound("2", 3));
    EXPECT_TRUE(matchesBound(" >= 1.5 ", 2));
    EXPECT_TRUE(matchesBound("-3", -3));
    EXPECT_TRUE(matchesBound("1e1", 10));
    // Malformed bounds never match.
    EXPECT_FALSE(matchesBound("abc", 0));
    EXPECT_FALSE(matchesBound(">=", 0));
    EXPECT_FALSE(matchesBound("=>5", 6));
    EXPECT_FALSE(parseBound("").has_value());
}

TEST(ScriptMatch, Score) {
    auto s = parseScore("kills>=3");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->objective, "kills");
    EXPECT_TRUE(s->bound.test(3));
    EXPECT_FALSE(s->bound.test(2));
    auto eq = parseScore("money=10");
    ASSERT_TRUE(eq);
    EXPECT_EQ(eq->objective, "money");
    EXPECT_TRUE(eq->bound.test(10));
    auto lt = parseScore(" a.b <2 ");
    ASSERT_TRUE(lt);
    EXPECT_EQ(lt->objective, "a.b");
    EXPECT_TRUE(lt->bound.test(1));
    EXPECT_FALSE(parseScore("kills").has_value());
    EXPECT_FALSE(parseScore(">=3").has_value());
    EXPECT_FALSE(parseScore("kills>=x").has_value());
}

TEST(ScriptMatch, Position) {
    auto p = parsePosition("x=10,y=60,z=-5,dx=5,dy=2,dz=3");
    ASSERT_TRUE(p);
    EXPECT_TRUE(positionMatches(*p, 10, 60, -5));
    EXPECT_TRUE(positionMatches(*p, 15, 62, -2));
    EXPECT_FALSE(positionMatches(*p, 16, 60, -5));
    EXPECT_FALSE(positionMatches(*p, 9, 60, -5));
    EXPECT_FALSE(positionMatches(*p, 10, 63, -5));
    EXPECT_FALSE(positionMatches(*p, 10, 60, -6));
    // Carry On quirk: an axis whose base is 0 is not checked.
    auto q = parsePosition("x=0,y=60,dx=1,dy=0");
    ASSERT_TRUE(q);
    EXPECT_TRUE(positionMatches(*q, 1000, 60, 1234));
    EXPECT_FALSE(positionMatches(*q, 0, 61, 0));
    auto none = parsePosition("");
    ASSERT_TRUE(none);
    EXPECT_TRUE(positionMatches(*none, 5, 5, 5));
    EXPECT_FALSE(parsePosition("x=1,w=2").has_value());
    EXPECT_FALSE(parsePosition("x").has_value());
    EXPECT_FALSE(parsePosition("x=a").has_value());
}

TEST(ScriptMatch, Xyz) {
    auto t = parseXyz("y=0.5", 0);
    ASSERT_TRUE(t);
    EXPECT_EQ((*t)[0], 0);
    EXPECT_EQ((*t)[1], 0.5);
    auto s = parseXyz("x=2", 1);
    ASSERT_TRUE(s);
    EXPECT_EQ((*s)[0], 2);
    EXPECT_EQ((*s)[2], 1);
    EXPECT_FALSE(parseXyz("q=1", 0).has_value());
}

TEST(ScriptMatch, Effects) {
    EXPECT_EQ(effectId("speed"), 1);
    EXPECT_EQ(effectId("minecraft:haste"), 3);
    EXPECT_EQ(effectId("Minecraft:Slowness"), 2);
    EXPECT_EQ(effectId("hero_of_the_village"), 29);
    EXPECT_EQ(effectId("village_hero"), 29);
    EXPECT_EQ(effectId("jump_boost"), 8);
    EXPECT_EQ(effectId("slow_falling"), 27);
    EXPECT_EQ(effectId("luck"), -1);
    EXPECT_EQ(effectId("glowing"), -1);
    EXPECT_EQ(effectId("dolphins_grace"), -1);
    auto e = parseEffects("minecraft:speed#1, haste ,luck#2");
    ASSERT_TRUE(e);
    ASSERT_EQ(e->size(), 3u);
    EXPECT_EQ((*e)[0].id, 1);
    EXPECT_EQ((*e)[0].amplifier, 1);
    EXPECT_EQ((*e)[1].name, "haste");
    EXPECT_EQ((*e)[1].amplifier, 0);
    EXPECT_EQ((*e)[2].id, -1);
    EXPECT_FALSE(parseEffects("speed#x").has_value());
    EXPECT_TRUE(parseEffects("")->empty());
}

TEST(ScriptMatch, GameMode) {
    EXPECT_EQ(javaGameModeId(0), 0);
    EXPECT_EQ(javaGameModeId(1), 1);
    EXPECT_EQ(javaGameModeId(2), 2);
    EXPECT_EQ(javaGameModeId(6), 3);
    EXPECT_EQ(javaGameModeId(5), -1);
}

TEST(ScriptMatch, Snbt) {
    auto j = parseSnbt(R"({id:"Chest",Items:[{Count:7b,Name:"minecraft:diamond",Slot:0b}],x:1,y:-2,z:3L,f:0.5f,d:1.25d,"minecraft:cardinal_direction":"south",bytes:[B;1b,2b],ints:[I;1,2],longs:[L;5L],esc:"a\"b\\c",single:'x',flag:true,bare:abc,empty:{},none:[]})");
    ASSERT_TRUE(j);
    EXPECT_EQ((*j)["id"], "Chest");
    EXPECT_EQ((*j)["Items"][0]["Count"], 7);
    EXPECT_EQ((*j)["Items"][0]["Name"], "minecraft:diamond");
    EXPECT_EQ((*j)["y"], -2);
    EXPECT_EQ((*j)["z"], 3);
    EXPECT_DOUBLE_EQ((*j)["f"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ((*j)["d"].get<double>(), 1.25);
    EXPECT_EQ((*j)["minecraft:cardinal_direction"], "south");
    EXPECT_EQ((*j)["bytes"], json::array({1, 2}));
    EXPECT_EQ((*j)["ints"], json::array({1, 2}));
    EXPECT_EQ((*j)["longs"], json::array({5}));
    EXPECT_EQ((*j)["esc"], "a\"b\\c");
    EXPECT_EQ((*j)["single"], "x");
    EXPECT_EQ((*j)["flag"], 1);
    EXPECT_EQ((*j)["bare"], "abc");
    EXPECT_TRUE((*j)["empty"].is_object());
    EXPECT_TRUE((*j)["none"].is_array());
    EXPECT_TRUE(parseSnbt(" { a : 1 , b : [ 1 , 2 ] } ").has_value());
    EXPECT_TRUE(parseSnbt(R"({u:"\u00e9"})").has_value());
    EXPECT_EQ((*parseSnbt(R"({u:"\u00e9"})"))["u"], "\xC3\xA9");
    EXPECT_FALSE(parseSnbt("").has_value());
    // Bedrock writes a comment after base64 strings (e.g. an entity's EntityStorageKey).
    auto c = parseSnbt(R"({k:{StorageKey:"AAAAAQAAAN4=" /*BASE64*/},n:1})");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ((*c)["k"]["StorageKey"], "AAAAAQAAAN4=");
    EXPECT_EQ((*c)["n"], 1);
    EXPECT_FALSE(parseSnbt("{a:1").has_value());
    EXPECT_FALSE(parseSnbt("{a:1}}").has_value());
    EXPECT_FALSE(parseSnbt("{a:\"x}").has_value());
    EXPECT_FALSE(parseSnbt("{:1}").has_value());
}

TEST(ScriptMatch, NbtMatches) {
    auto actual = *parseSnbt(R"({id:"Chest",Items:[{Count:7b,Name:"minecraft:diamond"},{Count:1b,Name:"minecraft:stick"}],CustomName:"Box",num:2.5f,list:[]})");
    EXPECT_TRUE(nbtMatches(json::object(), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"id":"Chest"})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"id":"Barrel"})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"missing":1})"), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"Items":[{"Name":"minecraft:stick"}]})"), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"Items":[{"Name":"minecraft:stick"},{"Count":7}]})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"Items":[{"Name":"minecraft:dirt"}]})"), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"num":2.5})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"num":"2.5"})"), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"list":[]})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"Items":[]})"), actual));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"id":{"a":1}})"), actual));
    EXPECT_TRUE(nbtMatches(json::parse(R"({"b":true})"), json::parse(R"({"b":1})")));
    EXPECT_FALSE(nbtMatches(json::parse(R"({"b":true})"), json::parse(R"({"b":0})")));
}

TEST(ScriptMatch, ParseScriptValidity) {
    std::string err;
    auto        ok = parseScript(json::parse(R"({"object":{"block":{"name":"minecraft:chest"}},"effects":{"commandPickup":"say hi"}})"), "a", &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(ok->id, "a");
    EXPECT_TRUE(ok->isBlock);
    EXPECT_EQ(ok->commandPickup, "say hi");
    EXPECT_EQ(ok->priority, 0);
    EXPECT_FALSE(ok->overrideChecks);

    // override_checks alone is enough.
    EXPECT_TRUE(parseScript(json::parse(R"({"override_checks":true,"object":{"entity":{}}})"), "b"));
    // ... but false is not.
    EXPECT_FALSE(parseScript(json::parse(R"({"override_checks":false,"object":{"entity":{}}})"), "c", &err));
    EXPECT_FALSE(err.empty());
    // Exactly one of block / entity.
    EXPECT_FALSE(parseScript(json::parse(R"({"object":{"block":{},"entity":{}},"render":{}})"), "d"));
    EXPECT_FALSE(parseScript(json::parse(R"({"object":{},"render":{}})"), "e"));
    EXPECT_FALSE(parseScript(json::parse(R"({"render":{}})"), "f"));
    // Empty sections still count.
    EXPECT_TRUE(parseScript(json::parse(R"({"object":{"entity":{}},"conditions":{}})"), "g"));
    EXPECT_TRUE(parseScript(json::parse(R"({"object":{"entity":{}},"render":{}})"), "h"));
    // Types.
    EXPECT_FALSE(parseScript(json::parse(R"({"priority":"x","object":{"entity":{}},"render":{}})"), "i"));
    EXPECT_FALSE(parseScript(json::parse(R"({"object":{"block":{"nbt":"x"}},"render":{}})"), "j"));
    EXPECT_FALSE(parseScript(json::parse(R"({"object":{"block":{"name":"a[b"}},"render":{}})"), "k"));
    EXPECT_FALSE(parseScript(json::parse(R"([1,2])"), "l"));
    // Numbers are accepted as bounds.
    auto n = parseScript(json::parse(R"({"object":{"entity":{"width":1,"height":">2"}},"render":{"name_entity":"minecraft:pig"}})"), "m");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->width, "1");
    EXPECT_EQ(n->height, ">2");
    EXPECT_EQ(n->render["name_entity"], "minecraft:pig");
}

TEST(ScriptMatch, ParseScriptFields) {
    auto s = parseScript(
        json::parse(R"({
            "priority": 7, "override_checks": true,
            "object": {"block": {"name": "minecraft:*chest", "hardness": ">=2", "resistance": "<10", "nbt": {"id": "Chest"}}},
            "conditions": {"gamestage": "s", "advancement": "minecraft:story/mine_stone", "xp": ">=5", "gamemode": "0",
                           "scoreboard": "obj>=3", "position": "x=1", "effects": "speed#1"},
            "effects": {"commandPickup": "a", "commandLoop": "b", "commandPlace": "c"}
        })"),
        "dir/name"
    );
    ASSERT_TRUE(s);
    EXPECT_EQ(s->priority, 7);
    EXPECT_TRUE(s->overrideChecks);
    ASSERT_TRUE(s->name);
    EXPECT_TRUE(s->name->matchesId({"minecraft:trapped_chest", {}, {}}));
    EXPECT_EQ(s->hardness, ">=2");
    EXPECT_EQ(s->resistance, "<10");
    ASSERT_TRUE(s->nbt);
    EXPECT_EQ((*s->nbt)["id"], "Chest");
    EXPECT_EQ(s->gamestage, "s");
    EXPECT_EQ(s->xp, ">=5");
    EXPECT_EQ(s->gamemode, "0");
    EXPECT_EQ(s->scoreboard, "obj>=3");
    EXPECT_EQ(s->position, "x=1");
    EXPECT_EQ(s->effects, "speed#1");
    EXPECT_TRUE(s->hasConditions());
    EXPECT_EQ(s->commandLoop, "b");
    EXPECT_TRUE(s->render.is_null());
}

TEST(ScriptMatch, SortByPriority) {
    std::vector<Script> v(4);
    v[0].id = "a", v[0].priority = 0;
    v[1].id = "b", v[1].priority = 5;
    v[2].id = "c", v[2].priority = 0;
    v[3].id = "d", v[3].priority = -1;
    sortScripts(v);
    EXPECT_EQ(v[0].id, "b");
    EXPECT_EQ(v[1].id, "a"); // stable
    EXPECT_EQ(v[2].id, "c");
    EXPECT_EQ(v[3].id, "d");
}

TEST(ScriptMatch, CustomConditions) {
    auto c = parseCustomCondition(" minecraft:chest[facing_direction=2](iron_age) ");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->text, "minecraft:chest[facing_direction=2](iron_age)");
    EXPECT_EQ(c->stage, "iron_age");
    EXPECT_EQ(c->entry.text, "minecraft:chest");
    ASSERT_EQ(c->entry.states.size(), 1u);
    EXPECT_EQ(c->entry.states[0].name, "facing_direction");
    auto e = parseCustomCondition("minecraft:*(animals)");
    ASSERT_TRUE(e);
    EXPECT_TRUE(e->entry.matchesId({"minecraft:pig", {}, {}}));
    EXPECT_EQ(e->stage, "animals");
    EXPECT_FALSE(parseCustomCondition("minecraft:chest").has_value());
    EXPECT_FALSE(parseCustomCondition("minecraft:chest()").has_value());
    EXPECT_FALSE(parseCustomCondition("(stage)").has_value());
    EXPECT_FALSE(parseCustomCondition("minecraft:chest(a)(b)").has_value());
    EXPECT_EQ(stageTag("iron_age"), "carryon:stage:iron_age");
}
