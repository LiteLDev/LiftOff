#include <gtest/gtest.h>

#include <map>

#include "core/CarryLists.h"
#include "core/CarryMath.h"

using namespace lo::core::carry;

namespace {
Candidate id(std::string_view s) { return {s, {}, {}}; }

Candidate block(std::string_view s, std::map<std::string, std::string, std::less<>> const* states) {
    return {s, {}, [states](std::string_view name) -> std::optional<std::string> {
                if (auto it = states->find(name); it != states->end()) return it->second;
                return std::nullopt;
            }};
}
} // namespace

TEST(CarryLists, Wildcard) {
    EXPECT_TRUE(wildcardMatch("minecraft:*_door", "minecraft:wooden_door"));
    EXPECT_TRUE(wildcardMatch("minecraft:*_door", "minecraft:iron_door"));
    EXPECT_FALSE(wildcardMatch("minecraft:*_door", "minecraft:trapdoor"));
    EXPECT_TRUE(wildcardMatch("create:*", "create:anything"));
    EXPECT_TRUE(wildcardMatch("create*:*", "create_enchantment_industry:x"));
    EXPECT_FALSE(wildcardMatch("create*:*", "minecraft:create"));
    EXPECT_TRUE(wildcardMatch("thaumcraft:pillar*", "thaumcraft:pillar_arcane"));
    EXPECT_TRUE(wildcardMatch("sophisticatedstorage:*chest", "sophisticatedstorage:limited_chest"));
    // Pieces must appear in order, as substrings (Carry On semantics, not anchored).
    EXPECT_FALSE(wildcardMatch("b*a", "ab"));
    EXPECT_TRUE(wildcardMatch("*", "anything"));
}

TEST(CarryLists, ParseEntry) {
    auto e = parseEntry("  minecraft:chest ");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->kind, ListEntry::Kind::Id);
    EXPECT_EQ(e->text, "minecraft:chest");

    e = parseEntry("#minecraft:logs");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->kind, ListEntry::Kind::Tag);
    EXPECT_EQ(e->text, "minecraft:logs");

    e = parseEntry("minecraft:*_bed");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->kind, ListEntry::Kind::Wildcard);
    ASSERT_EQ(e->pieces.size(), 2u);
    EXPECT_EQ(e->pieces[0], "minecraft:");
    EXPECT_EQ(e->pieces[1], "_bed");

    e = parseEntry("minecraft:big_dripleaf[big_dripleaf_head=false]");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->text, "minecraft:big_dripleaf");
    ASSERT_EQ(e->states.size(), 1u);
    EXPECT_EQ(e->states[0].name, "big_dripleaf_head");
    EXPECT_EQ(e->states[0].value, "false");

    e = parseEntry("minecraft:vine[north, east,south]");
    ASSERT_TRUE(e);
    ASSERT_EQ(e->states.size(), 3u);
    EXPECT_EQ(e->states[1].name, "east");
    EXPECT_FALSE(e->states[1].value);

    EXPECT_FALSE(parseEntry(""));
    EXPECT_FALSE(parseEntry("   "));
    EXPECT_FALSE(parseEntry("#"));
    EXPECT_FALSE(parseEntry("minecraft:chest[type"));
    EXPECT_FALSE(parseEntry("minecraft:chest]"));
    EXPECT_FALSE(parseEntry("[type]"));
}

TEST(CarryLists, SplitSpec) {
    auto s = splitSpec("minecraft:chest[facing=north](stage_one)");
    EXPECT_EQ(s.spec, "minecraft:chest[facing=north]");
    EXPECT_EQ(s.rest, "(stage_one)");
    s = splitSpec("minecraft:redstone_wire->(item)minecraft:redstone");
    EXPECT_EQ(s.spec, "minecraft:redstone_wire");
    EXPECT_EQ(s.rest, "->(item)minecraft:redstone");
    s = splitSpec("minecraft:chest{Items:[]}->minecraft:stone");
    EXPECT_EQ(s.spec, "minecraft:chest");
    EXPECT_EQ(s.rest, "{Items:[]}->minecraft:stone");
    s = splitSpec("minecraft:pig(stage)");
    EXPECT_EQ(s.spec, "minecraft:pig");
    EXPECT_EQ(s.rest, "(stage)");
}

TEST(CarryLists, Blacklist) {
    IdList black({"minecraft:bed", "minecraft:*_door", "#monster", "minecraft:big_dripleaf[big_dripleaf_head=false]"});
    IdList white;
    EXPECT_FALSE(isAllowed(black, white, false, id("minecraft:bed")));
    EXPECT_FALSE(isAllowed(black, white, false, id("minecraft:spruce_door")));
    EXPECT_TRUE(isAllowed(black, white, false, id("minecraft:chest")));

    Candidate zombie{"minecraft:zombie", [](std::string_view t) { return t == "monster"; }, {}};
    EXPECT_FALSE(isAllowed(black, white, false, zombie));

    std::map<std::string, std::string, std::less<>> stem{{"big_dripleaf_head", "false"}};
    std::map<std::string, std::string, std::less<>> head{{"big_dripleaf_head", "true"}};
    EXPECT_FALSE(isAllowed(black, white, false, block("minecraft:big_dripleaf", &stem)));
    EXPECT_TRUE(isAllowed(black, white, false, block("minecraft:big_dripleaf", &head)));
    // A filter on a state the candidate lacks never matches.
    EXPECT_TRUE(isAllowed(black, white, false, id("minecraft:big_dripleaf")));
}

TEST(CarryLists, WhitelistIgnoresBlacklist) {
    IdList black({"minecraft:chest"});
    IdList white({"minecraft:chest", "minecraft:barrel"});
    EXPECT_TRUE(isAllowed(black, white, true, id("minecraft:chest")));
    EXPECT_TRUE(isAllowed(black, white, true, id("minecraft:barrel")));
    EXPECT_FALSE(isAllowed(black, white, true, id("minecraft:furnace")));
    EXPECT_FALSE(isAllowed(black, IdList{}, true, id("minecraft:furnace")));
}

TEST(CarryLists, InvalidEntriesReported) {
    std::vector<std::string> invalid;
    IdList                   list({"minecraft:chest", "bad[", "", "#"}, &invalid);
    EXPECT_EQ(list.entries().size(), 1u);
    ASSERT_EQ(invalid.size(), 2u);
    EXPECT_EQ(invalid[0], "bad[");
}

TEST(CarryLists, StateValues) {
    EXPECT_TRUE(stateValueEquals("false", "0"));
    EXPECT_TRUE(stateValueEquals("TRUE", "1"));
    EXPECT_TRUE(stateValueEquals("double_top", "double_top"));
    EXPECT_FALSE(stateValueEquals("double_top", "double_bot"));
    EXPECT_FALSE(stateValueEquals("2", "true"));
}

TEST(CarryLists, PlacementExceptions) {
    std::vector<std::string> invalid;
    StateExceptions          ex(
        {"minecraft:stone_button[facing_direction]", "minecraft:vine[vine_direction_bits]", "minecraft:chest"},
        &invalid
    );
    EXPECT_TRUE(ex.contains("facing_direction"));
    EXPECT_TRUE(ex.contains("vine_direction_bits"));
    EXPECT_FALSE(ex.contains("upside_down_bit"));
    ASSERT_EQ(invalid.size(), 1u); // an entry without states names nothing
}

TEST(CarryMath, BlockSlowness) {
    EXPECT_EQ(blockSlowness(150, true, 1.0), 0);
    EXPECT_EQ(blockSlowness(999, true, 1.0), 1);
    EXPECT_EQ(blockSlowness(2400, true, 1.0), 4);
    EXPECT_EQ(blockSlowness(2400, true, 0.5), 2);
    EXPECT_EQ(blockSlowness(100000, true, 1.0), 4); // capped
    EXPECT_EQ(blockSlowness(100000, false, 1.0), 1);
    EXPECT_EQ(blockSlowness(1000, true, 0.0), 0);
}

TEST(CarryMath, EntitySlowness) {
    EXPECT_EQ(entitySlowness(0.9f, 0.9f, 1.f, true, 1.0), 0); // pig
    EXPECT_EQ(entitySlowness(1.3f, 0.9f, 1.f, true, 1.0), 1); // cow
    EXPECT_EQ(entitySlowness(1.6f, 1.4f, 1.f, true, 1.0), 2); // horse
    EXPECT_EQ(entitySlowness(1.3f, 0.9f, 1.f, true, 2.0), 2);
    EXPECT_EQ(entitySlowness(0.9f, 0.9f, 2.f, true, 1.0), 3); // carrier scale 0.5
    EXPECT_EQ(entitySlowness(5.f, 5.f, 1.f, true, 1.0), 4);
    EXPECT_EQ(entitySlowness(5.f, 5.f, 1.f, false, 1.0), 1);
}

TEST(CarryMath, Reach) {
    EXPECT_DOUBLE_EQ(reachScale(true, false, false), 1.0);
    EXPECT_DOUBLE_EQ(reachScale(false, true, true), 1.0);
    EXPECT_DOUBLE_EQ(reachScale(true, true, false), 5.0 / 4.5);
    EXPECT_DOUBLE_EQ(reachScale(true, true, true), 5.0 / 3.0);
    EXPECT_TRUE(withinReach(6.0, 2.5, 1.0));
    EXPECT_FALSE(withinReach(6.25, 2.5, 1.0)); // strictly less
    EXPECT_TRUE(withinReach(7.0, 2.5, 5.0 / 4.5));
}
