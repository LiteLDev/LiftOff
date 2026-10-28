#include <gtest/gtest.h>

#include "core/ModelOverride.h"

using namespace lo::core::modeloverride;
using lo::core::carry::ListEntry;
using lo::core::carry::StateFilter;

TEST(ModelOverride, DefaultEntries) {
    auto a = parse("minecraft:redstone_wire->(item)minecraft:redstone");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->source.kind, ListEntry::Kind::Id);
    EXPECT_EQ(a->source.text, "minecraft:redstone_wire");
    EXPECT_TRUE(a->nbt.empty());
    EXPECT_EQ(a->kind, TargetKind::Item);
    EXPECT_EQ(a->target, "minecraft:redstone");
    EXPECT_EQ(a->targetText(), "(item)minecraft:redstone");

    auto b = parse("minecraft:bamboo_sapling->(block)minecraft:bamboo");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->kind, TargetKind::Block);
    EXPECT_EQ(b->target, "minecraft:bamboo");
    EXPECT_TRUE(b->targetStates.empty());
    EXPECT_EQ(b->targetText(), "(block)minecraft:bamboo");
}

TEST(ModelOverride, DefaultKindIsItem) {
    auto o = parse("minecraft:chest->minecraft:diamond");
    ASSERT_TRUE(o);
    EXPECT_EQ(o->kind, TargetKind::Item);
    EXPECT_EQ(o->target, "minecraft:diamond");
}

TEST(ModelOverride, KindIsCaseInsensitiveAndWhitespaceTolerant) {
    auto o = parse("  minecraft:chest  ->  (BLOCK) minecraft:barrel ");
    ASSERT_TRUE(o);
    EXPECT_EQ(o->kind, TargetKind::Block);
    EXPECT_EQ(o->target, "minecraft:barrel");
    EXPECT_EQ(o->text, "minecraft:chest  ->  (BLOCK) minecraft:barrel");
}

TEST(ModelOverride, SourceStatesAndNbt) {
    auto o = parse(R"(minecraft:chest[facing_direction=2]{Items:[{Name:"minecraft:apple"}],CustomName:"a->b"}->(block)minecraft:barrel[facing_direction=1,open_bit=true])");
    ASSERT_TRUE(o);
    EXPECT_EQ(o->source.text, "minecraft:chest");
    ASSERT_EQ(o->source.states.size(), 1u);
    EXPECT_EQ(o->source.states[0], (StateFilter{"facing_direction", "2"}));
    EXPECT_EQ(o->nbt, R"({Items:[{Name:"minecraft:apple"}],CustomName:"a->b"})");
    EXPECT_EQ(o->kind, TargetKind::Block);
    EXPECT_EQ(o->target, "minecraft:barrel");
    ASSERT_EQ(o->targetStates.size(), 2u);
    EXPECT_EQ(o->targetStates[1], (StateFilter{"open_bit", "true"}));
    EXPECT_EQ(o->targetText(), "(block)minecraft:barrel[facing_direction=1,open_bit=true]");
}

TEST(ModelOverride, NbtWithoutStates) {
    auto o = parse("minecraft:furnace{BurnTime:0s}->minecraft:coal");
    ASSERT_TRUE(o);
    EXPECT_EQ(o->source.text, "minecraft:furnace");
    EXPECT_TRUE(o->source.states.empty());
    EXPECT_EQ(o->nbt, "{BurnTime:0s}");
}

TEST(ModelOverride, WildcardAndTagSources) {
    auto w = parse("minecraft:*_candle_cake->(block)minecraft:cake");
    ASSERT_TRUE(w);
    EXPECT_EQ(w->source.kind, ListEntry::Kind::Wildcard);
    EXPECT_TRUE(w->source.matchesId({"minecraft:white_candle_cake", {}, {}}));
    auto t = parse("#wood->minecraft:stick");
    ASSERT_TRUE(t);
    EXPECT_EQ(t->source.kind, ListEntry::Kind::Tag);
    EXPECT_EQ(t->source.text, "wood");
}

TEST(ModelOverride, MissingNamespaceDefaultsToMinecraft) {
    auto o = parse("redstone_wire->(item)redstone");
    ASSERT_TRUE(o);
    EXPECT_EQ(o->source.text, "minecraft:redstone_wire");
    EXPECT_EQ(o->target, "minecraft:redstone");
    auto b = parse("chest->(block)barrel[facing_direction=3]");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->target, "minecraft:barrel");
}

TEST(ModelOverride, InvalidEntries) {
    for (auto const* text : {
             "",
             "minecraft:chest",                         // no arrow
             "->minecraft:stone",                       // empty source
             "minecraft:chest->",                       // empty target
             "minecraft:chest->(item)",                 // empty target
             "minecraft:chest->(entity)minecraft:pig",  // unknown kind
             "minecraft:chest{a:1->minecraft:stone",    // unbalanced nbt
             "minecraft:chest{a:1}x->minecraft:stone",  // junk after nbt
             "minecraft:chest[facing]->minecraft:stone", // state without value
             "minecraft:chest[a=1->minecraft:stone",    // unbalanced states
             "minecraft:chest->(item)minecraft:stone[a=1]", // states on an item
             "minecraft:chest->(block)minecraft:*",     // wildcard target
             "minecraft:chest->#wood",                  // tag target
             "minecraft:chest->(block)minecraft:barrel[open_bit]", // target state without value
             "minecraft:chest->minecraft:stone{a:1}",   // nbt on target
             "minecraft:chest(stage)->minecraft:stone", // stage suffix
         }) {
        EXPECT_FALSE(parse(text)) << text;
    }
}

TEST(ModelOverride, ParseAllKeepsOrderAndReportsInvalid) {
    std::vector<std::string> invalid;
    auto                     all = parseAll({"a->b", "bad", "  ", "c->(block)d"}, &invalid);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].source.text, "minecraft:a");
    EXPECT_EQ(all[1].target, "minecraft:d");
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid[0], "bad");
}

TEST(ModelOverride, BlockSpec) {
    auto s = parseBlockSpec("stone[stone_type=granite]");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->id, "minecraft:stone");
    ASSERT_EQ(s->states.size(), 1u);
    EXPECT_EQ(s->states[0], (StateFilter{"stone_type", "granite"}));
    EXPECT_FALSE(parseBlockSpec(""));
    EXPECT_FALSE(parseBlockSpec("minecraft:*"));
    EXPECT_FALSE(parseBlockSpec("minecraft:stone[a]"));
    EXPECT_FALSE(parseBlockSpec("minecraft:stone[a=1]x"));
}

TEST(ScriptRender, Axes) {
    auto a = parseAxes("x=1,y=0.5,z=-2");
    EXPECT_EQ(a[0], 1.f);
    EXPECT_EQ(a[1], 0.5f);
    EXPECT_EQ(a[2], -2.f);
    auto b = parseAxes(" Y = +3 , junk, z=abc, x=1, x=2");
    EXPECT_EQ(b[0], 2.f);
    EXPECT_EQ(b[1], 3.f);
    EXPECT_FALSE(b[2]);
    auto c = parseAxes("");
    EXPECT_FALSE(c[0] || c[1] || c[2]);
    EXPECT_EQ(parseVector("y=2", 1.f), (std::array{1.f, 2.f, 1.f}));
    EXPECT_EQ(parseVector("x=0.25", 0.f), (std::array{0.25f, 0.f, 0.f}));
}

TEST(ScriptRender, FullObject) {
    auto j = nlohmann::json::parse(R"({
        "name_block": "stone[stone_type=granite]",
        "name_entity": "pig",
        "nbt": {"IsBaby": true, "Variant": 2, "Name": "x"},
        "translation": "x=0,y=0.5,z=0",
        "rotation": "y=90",
        "scale": "x=2",
        "rotation_left_arm": "x=1.2",
        "rotation_right_arm": "x=1.3,z=0.1",
        "render_left_arm": false,
        "render_right_arm": true
    })");
    auto r = parseScriptRender(j);
    EXPECT_EQ(r.nameBlock, "minecraft:stone[stone_type=granite]");
    EXPECT_EQ(r.nameEntity, "minecraft:pig");
    EXPECT_EQ(r.nbt, R"({IsBaby:1b,Name:"x",Variant:2})");
    EXPECT_EQ(r.translation, (std::array{0.f, 0.5f, 0.f}));
    EXPECT_EQ(r.rotation, (std::array{0.f, 90.f, 0.f}));
    EXPECT_EQ(r.scale, (std::array{2.f, 1.f, 1.f}));
    EXPECT_EQ(r.leftArm[0], 1.2f);
    EXPECT_FALSE(r.leftArm[1]);
    EXPECT_FALSE(r.leftArm[2]);
    EXPECT_EQ(r.rightArm[0], 1.3f);
    EXPECT_EQ(r.rightArm[2], 0.1f);
    EXPECT_FALSE(r.renderLeftArm);
    EXPECT_TRUE(r.renderRightArm);
    EXPECT_FALSE(r.empty());
}

TEST(ScriptRender, DefaultsAndBadInput) {
    EXPECT_TRUE(parseScriptRender(nlohmann::json()).empty());
    EXPECT_TRUE(parseScriptRender(nlohmann::json::array()).empty());
    EXPECT_TRUE(parseScriptRender(nlohmann::json::object()).empty());
    auto r = parseScriptRender(nlohmann::json::parse(
        R"({"name_block": "", "name_entity": 5, "nbt": {}, "scale": 3, "render_left_arm": "no", "translation": "x=1"})"
    ));
    EXPECT_FALSE(r.nameBlock);
    EXPECT_FALSE(r.nameEntity);
    EXPECT_FALSE(r.nbt);
    EXPECT_EQ(r.scale, (std::array{1.f, 1.f, 1.f}));
    EXPECT_TRUE(r.renderLeftArm);
    EXPECT_EQ(r.translation, (std::array{1.f, 0.f, 0.f}));
    EXPECT_FALSE(r.empty());
}

TEST(ScriptRender, NbtAsSnbtString) {
    auto r = parseScriptRender(nlohmann::json::parse(R"({"nbt": " {Saddled:1b} "})"));
    EXPECT_EQ(r.nbt, "{Saddled:1b}");
    auto bad = parseScriptRender(nlohmann::json::parse(R"({"nbt": "{Saddled:1b"})"));
    EXPECT_FALSE(bad.nbt);
}

TEST(ScriptRender, JsonToSnbt) {
    auto j = nlohmann::json::parse(R"({"a": [1, 2], "b": 1.5, "c": 3000000000, "d": "q\"\\", "e e": false, "f": null})");
    EXPECT_EQ(jsonToSnbt(j), R"({a:[1,2],b:1.5d,c:3000000000L,d:"q\"\\","e e":0b,f:0b})");
    EXPECT_EQ(jsonToSnbt(nlohmann::json::object()), "{}");
    EXPECT_EQ(jsonToSnbt(nlohmann::json(-7)), "-7");
}
