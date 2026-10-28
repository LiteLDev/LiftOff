// Selftests of U12 (model overrides + script render, dx 1200..1299; none of them touches the world).

#include <algorithm>
#include <atomic>
#include <memory>

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Hooks.h"
#include "feature/modeloverrides/ModelOverrides.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

#ifdef LO_CLIENT
#include "ll/api/thread/ClientThreadExecutor.h"
#endif

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace m     = lo::feature::modeloverrides;
using carry::CarryData;
using carry::CarryType;
using carry::CarryVisual;

std::string snbtOf(Context& ctx, std::string const& id, std::vector<std::pair<std::string_view, int>> const& states = {}) {
    auto const* b = lo::game::blockWith(id, states);
    ctx.require(b != nullptr, id + " exists");
    return carry::toSnbt(*b->mSerializationId);
}

CarryData blockCarry(Context& ctx, std::string const& id, std::string blockEntity = {}) {
    CarryData d;
    d.type        = CarryType::Block;
    d.block       = snbtOf(ctx, id);
    d.blockEntity = std::move(blockEntity);
    return d;
}

std::optional<m::OverrideResult>
find(Context& ctx, std::vector<m::CompiledOverride> const& list, std::string const& blockSnbt, std::string const& be = {}) {
    auto tag = carry::parseNbt(blockSnbt);
    ctx.require(tag.has_value(), "block SNBT parses");
    auto block = Block::tryGetFromRegistry(*tag);
    ctx.require(static_cast<bool>(block), "block SNBT resolves");
    auto beTag = carry::parseNbt(be);
    return m::findOverride(list, *block.as_ptr(), beTag ? &*beTag : nullptr);
}

std::string nameOf(std::string const& blockSnbt) {
    auto tag = carry::parseNbt(blockSnbt);
    if (!tag || !tag->contains("name")) return {};
    return std::string(static_cast<std::string const&>(tag->at("name").get<StringTag>()));
}

bool contains(std::string const& s, std::string_view part) { return s.find(part) != std::string::npos; }

CarryVisual baseVisual(CarryData const& d) {
    CarryVisual v;
    if (d.isBlock()) {
        v.kind  = CarryVisual::Kind::Block;
        v.block = d.block;
    } else if (d.isEntity()) {
        v.kind   = CarryVisual::Kind::Entity;
        v.entity = d.entity;
    }
    return v;
}

/// Sets the configured model overrides for one test; restores the user's list (and recompiles) on exit,
/// also when the test aborts.
class OverridesGuard {
public:
    explicit OverridesGuard(std::vector<std::string> entries)
    : mSaved(lo::config::get<carry::CarryClientConfig>().modelOverrides.modelOverrides) {
        set(std::move(entries));
    }
    ~OverridesGuard() { set(std::move(mSaved)); }
    OverridesGuard(OverridesGuard const&)            = delete;
    OverridesGuard& operator=(OverridesGuard const&) = delete;

    static void set(std::vector<std::string> entries) {
        lo::config::mut<carry::CarryClientConfig>().modelOverrides.modelOverrides = std::move(entries);
        m::rebuild();
    }

private:
    std::vector<std::string> mSaved;
};

constexpr char const* kChestBe =
    R"({id:"Chest",Findable:0b,Items:[{Count:1b,Name:"minecraft:apple",Slot:0b,WasPickedUp:0b},{Count:5b,Name:"minecraft:dirt",Slot:3b,WasPickedUp:0b}],x:0,y:0,z:0})";
} // namespace

LO_REGISTER_SELFTEST("modeloverrides.defaults", [](Context& ctx) {
    auto const cfg = carry::CarryClientConfig{}.modelOverrides.modelOverrides;
    std::vector<std::string> invalid;
    auto                     list = m::compile(cfg, &invalid);
    ctx.check(invalid.empty(), "the default entries are valid");
    ctx.require(list.size() == 3, "3 default overrides");

    auto wire = find(ctx, list, snbtOf(ctx, "minecraft:redstone_wire"));
    ctx.check(wire && wire->kind == CarryVisual::Kind::Item && wire->item == "minecraft:redstone", "redstone_wire -> item redstone");
    ctx.check(wire && wire->target == "(item)minecraft:redstone", "target text of redstone_wire");

    auto sapling = find(ctx, list, snbtOf(ctx, "minecraft:bamboo_sapling"));
    ctx.check(sapling && sapling->kind == CarryVisual::Kind::Block && nameOf(sapling->block) == "minecraft:bamboo", "bamboo_sapling -> block bamboo");

    auto cake = find(ctx, list, snbtOf(ctx, "minecraft:candle_cake"));
    ctx.check(cake && cake->kind == CarryVisual::Kind::Block && nameOf(cake->block) == "minecraft:cake", "candle_cake -> block cake");

    ctx.check(!find(ctx, list, snbtOf(ctx, "minecraft:chest")), "a chest has no override");
});

LO_REGISTER_SELFTEST("modeloverrides.forms", [](Context& ctx) {
    std::vector<std::string> invalid;
    auto list = m::compile(
        {
            "minecraft:stone_button[facing_direction=1]->(item)minecraft:stick",        // 0 state filter
            "minecraft:chest{Items:[{Name:\"minecraft:apple\"}]}->minecraft:apple",      // 1 NBT, default item
            "minecraft:chest{Findable:0b}->(block)minecraft:barrel[facing_direction=1,open_bit=true]", // 2 block target + states
            "minecraft:*_candle_cake->(block)minecraft:cake",                            // 3 wildcard
            "minecraft:dirt->(item)minecraft:not_a_real_item",                           // 4 unknown target: skipped
            "minecraft:dirt->(item)minecraft:diamond",                                   // 5
            "minecraft:dirt->(item)minecraft:emerald",                                   // 6 never reached
            "minecraft:gravel->(block)minecraft:barrel[no_such_state=1]",                // 7 unknown target state: skipped
            "gravel->(block)stone",                                                      // 8 namespace default
            "minecraft:sand{bad->minecraft:stick",                                       // invalid syntax
            "minecraft:sand{a:[1b,}->minecraft:stick",                                   // invalid SNBT
        },
        &invalid
    );
    ctx.check(invalid.size() == 2, "invalid syntax and invalid SNBT are reported (" + std::to_string(invalid.size()) + ")");
    ctx.require(list.size() >= 9, "9 valid entries");

    auto pressed = find(ctx, list, snbtOf(ctx, "minecraft:stone_button", {{"facing_direction", 1}}));
    ctx.check(pressed && pressed->index == 0 && pressed->item == "minecraft:stick", "state filter matches");
    ctx.check(!find(ctx, list, snbtOf(ctx, "minecraft:stone_button", {{"facing_direction", 2}})), "other states do not match");

    auto chest = snbtOf(ctx, "minecraft:chest");
    auto apple = find(ctx, list, chest, kChestBe);
    ctx.check(apple && apple->index == 1 && apple->kind == CarryVisual::Kind::Item && apple->item == "minecraft:apple", "partial NBT (list element) matches; default kind is item");
    auto other = find(ctx, list, chest, R"({id:"Chest",Findable:0b,Items:[{Count:1b,Name:"minecraft:stone",Slot:0b}]})");
    ctx.check(other && other->index == 2, "first matching entry wins (NBT of entry 1 does not match)");
    ctx.check(other && other->kind == CarryVisual::Kind::Block && nameOf(other->block) == "minecraft:barrel", "block target");
    ctx.check(other && contains(other->block, "open_bit:1b") && contains(other->block, "facing_direction:1"), "block target states are set");
    ctx.check(!find(ctx, list, chest), "an NBT condition never matches a block without block actor");
    ctx.check(!find(ctx, list, chest, R"({id:"Chest",Findable:1b,Items:[]})"), "typed NBT values must be equal");

    if (lo::game::blockWith("minecraft:white_candle_cake")) {
        auto cake = find(ctx, list, snbtOf(ctx, "minecraft:white_candle_cake"));
        ctx.check(cake && cake->index == 3, "wildcard source matches");
    }

    auto dirt = find(ctx, list, snbtOf(ctx, "minecraft:dirt"));
    ctx.check(dirt && dirt->index == 5 && dirt->item == "minecraft:diamond", "an unknown target is skipped, the next entry wins");
    auto gravel = find(ctx, list, snbtOf(ctx, "minecraft:gravel"));
    ctx.check(gravel && gravel->index == 8 && nameOf(gravel->block) == "minecraft:stone", "unknown target state skipped; ids default to minecraft:");

    // #tag source: use a tag the log really has.
    auto const* oakLog = lo::game::blockWith("minecraft:oak_log");
    ctx.require(oakLog != nullptr, "oak_log exists");
    if (!oakLog->mTags->empty()) {
        auto tag    = std::string(oakLog->mTags->front().getString());
        auto tagged = m::compile({"#" + tag + "->(item)minecraft:stick"});
        ctx.require(tagged.size() == 1, "tag entry parses");
        auto r = find(ctx, tagged, snbtOf(ctx, "minecraft:oak_log"));
        ctx.check(r && r->item == "minecraft:stick", "tag source matches (#" + tag + ")");
    } else {
        ctx.info("oak_log has no block tags; tag case skipped");
    }
});

LO_REGISTER_SELFTEST("modeloverrides.nbt_compare", [](Context& ctx) {
    auto actual = carry::parseNbt(kChestBe);
    ctx.require(actual.has_value(), "chest NBT parses");
    auto check = [&](char const* expected, bool want) {
        auto e = carry::parseNbt(expected);
        ctx.require(e.has_value(), std::string("parses ") + expected);
        ctx.check(m::nbtPartialMatch(*e, *actual) == want, std::string(expected) + (want ? " matches" : " does not match"));
    };
    check("{}", true);
    check(R"({id:"Chest"})", true);
    check(R"({id:"Barrel"})", false);
    check("{Findable:0b}", true);
    check("{Findable:0}", false); // int vs byte
    check(R"({Items:[{Name:"minecraft:dirt",Count:5b}]})", true);
    check(R"({Items:[{Name:"minecraft:dirt",Count:4b}]})", false);
    check(R"({Items:[{Name:"minecraft:dirt"},{Name:"minecraft:apple"}]})", true);
    check("{Items:[]}", false); // an empty list only matches an empty list
    check("{Missing:1b}", false);
});

LO_REGISTER_SELFTEST("modeloverrides.script_render", [](Context& ctx) {
    // Block: name_block + transform + arms.
    auto d = blockCarry(ctx, "minecraft:chest");
    d.scriptRender = nlohmann::json::parse(R"({
        "name_block": "minecraft:barrel[open_bit=true]",
        "translation": "x=0,y=0.5,z=-1",
        "rotation": "y=90",
        "scale": "x=2,z=0.5",
        "rotation_left_arm": "x=1.2",
        "rotation_right_arm": "z=0.3",
        "render_left_arm": false
    })");
    auto v = baseVisual(d);
    m::applyVisual(d, v, false);
    ctx.check(v.kind == CarryVisual::Kind::Block && nameOf(v.block) == "minecraft:barrel" && contains(v.block, "open_bit:1b"), "name_block replaces the block (with states)");
    ctx.check(v.translation == std::array{0.f, 0.5f, -1.f}, "translation");
    ctx.check(v.rotation == std::array{0.f, 90.f, 0.f}, "rotation (degrees, missing axes 0)");
    ctx.check(v.scale == std::array{2.f, 1.f, 0.5f}, "scale (missing axes 1)");
    ctx.check(v.leftArm[0] == 1.2f && !v.leftArm[1] && !v.leftArm[2], "left arm replaces only the given axis");
    ctx.check(!v.rightArm[0] && !v.rightArm[1] && v.rightArm[2] == 0.3f, "right arm replaces only the given axis");
    ctx.check(!v.renderLeftArm && v.renderRightArm, "render_left_arm false, render_right_arm default true");

    // Unknown name_block: the block stays.
    d.scriptRender = {{"name_block", "minecraft:no_such_block"}};
    v              = baseVisual(d);
    m::applyVisual(d, v, false);
    ctx.check(v.block == d.block, "an unknown name_block keeps the carried block");

    // Entity: name_entity gives a fresh entity, nbt is merged.
    CarryData e;
    e.type   = CarryType::Entity;
    e.entity = R"({identifier:"minecraft:pig",Saddled:0b,UniqueID:42L,definitions:["+minecraft:pig"]})";
    e.scriptRender = nlohmann::json::parse(R"({"name_entity": "cow", "nbt": {"IsBaby": true}})");
    auto ev  = baseVisual(e);
    m::applyVisual(e, ev, true);
    auto tag = carry::parseNbt(ev.entity);
    ctx.require(tag.has_value(), "rendered entity SNBT parses");
    ctx.check(contains(ev.entity, R"(identifier:"minecraft:cow")") && contains(ev.entity, "IsBaby:1b"), "name_entity + nbt");
    ctx.check(!tag->contains("UniqueID") && !tag->contains("Saddled"), "name_entity draws a fresh entity");

    e.scriptRender = nlohmann::json::parse(R"({"nbt": "{Saddled:1b,Variant:3}"})");
    ev             = baseVisual(e);
    m::applyVisual(e, ev, true);
    ctx.check(contains(ev.entity, R"(identifier:"minecraft:pig")") && contains(ev.entity, "Saddled:1b") && contains(ev.entity, "Variant:3") && contains(ev.entity, "UniqueID:42L"), "nbt alone is merged into the carried entity");

    // No script render: nothing changes.
    e.scriptRender = nullptr;
    ev             = baseVisual(e);
    m::applyVisual(e, ev, true);
    ctx.check(ev.entity == e.entity && ev.scale == std::array{1.f, 1.f, 1.f} && ev.renderLeftArm, "no script render, no change");
});

LO_REGISTER_SELFTEST("modeloverrides.precedence", [](Context& ctx) {
    OverridesGuard guard({"minecraft:redstone_wire->(item)minecraft:redstone"});

    auto d = blockCarry(ctx, "minecraft:redstone_wire");
    // Model override alone (client behaviour).
    auto v = baseVisual(d);
    m::applyVisual(d, v, true);
    ctx.check(v.kind == CarryVisual::Kind::Item && v.item == "minecraft:redstone" && v.itemOverride, "item override sets Kind::Item + itemOverride");

    // Script name_block wins over the model override.
    d.scriptRender = {{"name_block", "minecraft:barrel"}};
    v              = baseVisual(d);
    m::applyVisual(d, v, true);
    ctx.check(v.kind == CarryVisual::Kind::Block && nameOf(v.block) == "minecraft:barrel" && !v.itemOverride, "script name_block wins");

    // A script without name_block keeps the override and adds its transform.
    d.scriptRender = {{"translation", "y=1"}};
    v              = baseVisual(d);
    m::applyVisual(d, v, true);
    ctx.check(v.kind == CarryVisual::Kind::Item && v.translation == std::array{0.f, 1.f, 0.f}, "override + script transform");

    // Without model overrides (dedicated server) nothing replaces the block.
    d.scriptRender = nullptr;
    v              = baseVisual(d);
    m::applyVisual(d, v, false);
    ctx.check(v.kind == CarryVisual::Kind::Block && v.block == d.block, "no model overrides on a dedicated server");

    // Re-parsed on reload: a changed list applies at once.
    OverridesGuard::set({"minecraft:redstone_wire->(item)minecraft:stick"});
    auto r = m::configuredOverride(d.block, d.blockEntity);
    ctx.check(r && r->item == "minecraft:stick", "the list is re-parsed on reload");
});

LO_REGISTER_SELFTEST("modeloverrides.provider", [](Context& ctx) -> ll::coro::CoroTask<> {
    OverridesGuard guard(carry::CarryClientConfig{}.modelOverrides.modelOverrides);
    auto           d = blockCarry(ctx, "minecraft:redstone_wire");

    // The server thread (stand-ins for vanilla clients, BDS) never applies model overrides.
    auto v = carry::visualFor(d);
    ctx.check(v.kind == CarryVisual::Kind::Block && v.block == d.block, "no model override on the server thread");

    auto lines   = carry::extraDebugLines(ctx.player(), d);
    bool hasLine = std::any_of(lines.begin(), lines.end(), [](std::string const& l) {
        return l.find("(item)minecraft:redstone") != std::string::npos;
    });
    ctx.check(hasLine == m::modelOverridesActive(), m::modelOverridesActive() ? "/carryon debug shows Override Model: (item)minecraft:redstone" : "no Override Model line on a dedicated server");
    auto chest      = blockCarry(ctx, "minecraft:chest");
    auto chestLines = carry::extraDebugLines(ctx.player(), chest);
    ctx.check(std::none_of(chestLines.begin(), chestLines.end(), [](std::string const& l) {
        return l.find("(item)") != std::string::npos || l.find("(block)") != std::string::npos;
    }), "no Override Model line without a matching override");

    chest.scriptRender = {{"scale", "x=3,y=3,z=3"}};
    auto sv            = carry::visualFor(chest);
    ctx.check(sv.scale == std::array{3.f, 3.f, 3.f}, "visualFor applies script render on the server thread");

#ifdef LO_CLIENT
    // The renderers' view: visualFor on the client thread applies the configured override.
    struct Result {
        std::atomic<bool> done{false};
        CarryVisual       visual;
    };
    auto result = std::make_shared<Result>();
    ll::thread::ClientThreadExecutor::getDefault().execute([result, d] {
        result->visual = carry::visualFor(d);
        result->done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 100 && !result->done.load(std::memory_order_acquire); ++i) co_await ctx.ticks(1);
    ctx.require(result->done.load(std::memory_order_acquire), "the client thread ran visualFor");
    auto const& cv = result->visual;
    ctx.check(cv.kind == CarryVisual::Kind::Item && cv.item == "minecraft:redstone" && cv.itemOverride, "visualFor applies the configured override on the client thread");
#endif
    co_return;
});
