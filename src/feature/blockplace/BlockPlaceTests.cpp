// Selftests of U2 blockplace (dx 200..299): placement target, state recompute against vanilla
// placement, kept states, can-place failures, protection, height, water, chests, skull rotation and
// the use-block path (cancelled interaction, deferred placement).

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/FloatTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/ChestBlockActor.h"

#include "feature/blockplace/BlockPlace.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace bp    = lo::feature::blockplace;
using bp::PlaceResult;
using carry::CarryData;
using carry::CarryType;
using Facing = ScriptModuleMinecraft::ScriptFacing;

constexpr int   kUp     = 1;
constexpr int   kWest   = 4;
constexpr float kEast   = -90.0f; // yaw looking east (+x)
constexpr float kSouth  = 0.0f;

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

/// Lifts the game's real-time delay between two builds of the same player (tests click faster).
void allowBuild(Player& p) { p.mGameMode.get()->mLastBuildTime = std::chrono::steady_clock::time_point{}; }

/// A vanilla right-click on a block with `item` (a first click, not a held-button repeat): the same
/// `GameMode::useItemOn` the network handler calls for a client (SimulatedPlayer::simulateInteract
/// bypasses it, simulateUseItemOnBlock only places the first block of a "continued" build).
/// `hit` is relative to `pos` (the game takes world coordinates).
void useItemOn(Player& p, ItemStack& item, BlockPos const& pos, Facing face, Vec3 const& hit) {
    allowBuild(p);
    Vec3 world{static_cast<float>(pos.x) + hit.x, static_cast<float>(pos.y) + hit.y, static_cast<float>(pos.z) + hit.z};
    p.mGameMode.get()->useItemOn(
        item,
        pos,
        static_cast<uchar>(face),
        world,
        HandSlot::Mainhand,
        &p.getDimensionBlockSource().getBlock(pos),
        true
    );
}

/// A right-click with the selected (empty) hand.
void rightClick(Player& p, BlockPos const& pos, Facing face, Vec3 const& hit) {
    ItemStack hand = p.getSelectedItem();
    useItemOn(p, hand, pos, face, hit);
}

lo::core::GlobalPos gp(BlockPos const& p) { return {0, p.x, p.y, p.z}; }
BlockPos            add(BlockPos const& p, int dx, int dy, int dz) { return BlockPos{p.x + dx, p.y + dy, p.z + dz}; }

/// A stone floor under `t` from x-3 to x+2 (z-1..z+1) and air above it (two blocks high).
void buildArea(Context& ctx, BlockPos const& t) {
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -3; dx <= 2; ++dx) {
            ctx.setBlock(gp(add(t, dx, -1, dz)), "minecraft:stone");
            ctx.setBlock(gp(add(t, dx, 0, dz)), "minecraft:air");
            ctx.setBlock(gp(add(t, dx, 1, dz)), "minecraft:air");
        }
    }
}

/// Puts the player at `feet` (block), facing `yaw`.
void standAt(Player& p, BlockPos const& feet, float yaw) {
    p.teleport(
        Vec3{static_cast<float>(feet.x) + 0.5f, static_cast<float>(feet.y), static_cast<float>(feet.z) + 0.5f},
        p.getDimensionId(),
        Vec2{0.0f, yaw}
    );
}

std::string stateText(Block const& b, std::string_view name) { return carry::blockStateValue(b, name).value_or("<none>"); }

std::vector<std::string> stateNames(Block const& b) {
    std::vector<std::string> names;
    auto const&              tag = *b.mSerializationId;
    if (!tag.contains("states", Tag::Type::Compound)) return names;
    for (auto const& [name, value] : tag.at("states").get<CompoundTag>().mTags) {
        (void)value;
        names.push_back(name);
    }
    return names;
}

/// `block` with one state replaced (string or int, as the state already is).
Block const* withState(Block const& block, std::string const& name, std::string const& value) {
    CompoundTag tag = *block.mSerializationId;
    auto&       st  = tag.at("states").get<CompoundTag>();
    if (!st.contains(name)) return nullptr;
    if (st.at(name).hold(Tag::Type::String)) st.at(name) = StringTag(value);
    else if (st.at(name).hold(Tag::Type::Int)) st.at(name) = IntTag(std::stoi(value));
    else st.at(name) = ByteTag(static_cast<schar>(value == "true" || value == "1"));
    auto r = Block::tryGetFromRegistry(tag);
    return r ? r.as_ptr() : nullptr;
}

CarryData blockCarry(Block const& block, std::string blockEntity = {}) {
    CarryData d;
    d.type        = CarryType::Block;
    d.block       = carry::toSnbt(*block.mSerializationId);
    d.blockEntity = std::move(blockEntity);
    return d;
}

ChestBlockActor* chestAt(BlockSource& region, BlockPos const& pos) {
    auto* a = region.getBlockEntity(pos);
    return a && a->getType() == BlockActorType::Chest ? static_cast<ChestBlockActor*>(a) : nullptr;
}

/// Sets a block that `Context::setBlock` cannot express (string states); cleaned up after the test.
bool setState(Context& ctx, BlockPos const& pos, Block const* block) {
    if (!block) return false;
    ctx.cleanupBlock(gp(pos));
    return lo::game::setBlock(regionOf(ctx), pos, *block);
}
} // namespace

LO_REGISTER_SELFTEST("blockplace.recomputed_states", [](Context& ctx) {
    ctx.check(bp::isRecomputedState("minecraft:chest", "minecraft:cardinal_direction"), "cardinal_direction");
    ctx.check(bp::isRecomputedState("minecraft:furnace", "minecraft:cardinal_direction"), "furnace direction");
    ctx.check(bp::isRecomputedState("minecraft:oak_log", "pillar_axis"), "pillar_axis");
    ctx.check(bp::isRecomputedState("minecraft:oak_stairs", "weirdo_direction"), "weirdo_direction");
    ctx.check(bp::isRecomputedState("minecraft:barrel", "facing_direction"), "facing_direction");
    ctx.check(bp::isRecomputedState("minecraft:torch", "torch_facing_direction"), "torch_facing_direction");
    ctx.check(bp::isRecomputedState("minecraft:pointed_dripstone", "hanging"), "dripstone tip direction");
    ctx.check(!bp::isRecomputedState("minecraft:mangrove_propagule", "hanging"), "hanging of other blocks is kept");
    ctx.check(!bp::isRecomputedState("minecraft:rail", "rail_direction"), "rail_direction is kept");
    ctx.check(!bp::isRecomputedState("minecraft:crafter", "orientation"), "orientation is kept");
    ctx.check(!bp::isRecomputedState("minecraft:oak_slab", "minecraft:vertical_half"), "slab half is kept");
    ctx.check(!bp::isRecomputedState("minecraft:red_mushroom_block", "huge_mushroom_bits"), "mushroom bits are kept");
    // Default placementStateExceptions.
    ctx.check(bp::isRecomputedState("minecraft:oak_stairs", "upside_down_bit"), "exception: upside_down_bit");
    ctx.check(bp::isRecomputedState("minecraft:vine", "vine_direction_bits"), "exception: vine_direction_bits");
    ctx.check(bp::isRecomputedState("minecraft:standing_sign", "ground_sign_direction"), "exception: ground_sign_direction");
    ctx.check(bp::isRecomputedState("minecraft:bell", "attachment"), "exception: attachment (shared)");
});

LO_REGISTER_SELFTEST("blockplace.target", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(200));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto&    region = regionOf(ctx);
    BlockPos floor  = add(t, 0, -1, 0);
    ctx.check(bp::placementTarget(region, t, kUp) == t, "a replaceable click is the target");
    ctx.check(bp::placementTarget(region, floor, kUp) == t, "a solid click targets the face neighbour");
    ctx.check(bp::placementTarget(region, floor, kWest) == add(floor, -1, 0, 0), "west face neighbour");
    ctx.require(ctx.setBlock(gp(t), "minecraft:water"), "water set");
    ctx.check(bp::placementTarget(region, t, kUp) == t, "water is replaceable");
    ctx.check(bp::hasWaterSource(region, t), "a water source is seen");
});

// Facing recompute: the carried block takes the direction a vanilla placement by the same player
// would give it; the other states stay as carried.
LO_REGISTER_SELFTEST("blockplace.facing_vanilla", [](Context& ctx) -> ll::coro::CoroTask<> {
    struct Case {
        char const*              id;
        char const*              state;
        std::vector<std::string> values;
    };
    std::vector<Case> cases{
        {"minecraft:chest", "minecraft:cardinal_direction", {"north", "south", "east", "west"}},
        {"minecraft:furnace", "minecraft:cardinal_direction", {"north", "south", "east", "west"}},
        {"minecraft:oak_stairs", "weirdo_direction", {"0", "1", "2", "3"}},
        {"minecraft:oak_log", "pillar_axis", {"x", "y", "z"}},
        {"minecraft:barrel", "facing_direction", {"0", "1", "2", "3", "4", "5"}},
    };
    BlockPos t = lo::game::blockPos(ctx.testPos(210));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto&    p       = ctx.player();
    auto&    region  = regionOf(ctx);
    BlockPos support = add(t, 1, 0, 0); // clicked on its west face
    ctx.require(ctx.setBlock(gp(support), "minecraft:stone"), "support block");
    for (float yaw : {kEast, kSouth}) {
        for (auto const& c : cases) {
            standAt(p, add(t, -2, 0, 0), yaw);
            co_await ctx.ticks(2);
            ctx.setBlock(gp(t), "minecraft:air");
            auto item = lo::game::makeItem(c.id);
            useItemOn(p, item, support, Facing::West, Vec3{0.0f, 0.25f, 0.5f});
            co_await ctx.ticks(1);
            Block const& vanilla = region.getBlock(t);
            if (vanilla.getTypeName() != c.id) {
                ctx.check(false, std::string("vanilla placed ") + c.id + " (got " + vanilla.getTypeName() + ")");
                continue;
            }
            std::string want = stateText(vanilla, c.state);
            std::string other;
            for (auto const& v : c.values) {
                if (v != want) {
                    other = v;
                    break;
                }
            }
            Block const* stored = withState(vanilla, c.state, other);
            if (std::string(c.id) == "minecraft:oak_stairs" && stored) stored = withState(*stored, "upside_down_bit", "true");
            ctx.require(stored != nullptr, std::string("stored state for ") + c.id);
            ctx.setBlock(gp(t), "minecraft:air");
            Block const* ours = bp::placementState(p, region, t, *stored, kWest, p.getFeetPos());
            ctx.require(ours != nullptr, std::string("a placement state for ") + c.id);
            for (auto const& name : stateNames(vanilla)) {
                if (!bp::isRecomputedState(c.id, name)) continue;
                ctx.check(
                    stateText(*ours, name) == stateText(vanilla, name),
                    std::string(c.id) + " " + name + ": " + stateText(*ours, name) + " (vanilla " + stateText(vanilla, name) + ")"
                );
            }
            // The resolver in the spine is ours.
            ctx.check(&carry::resolvePlacementState(p, region, t, *stored, kWest, p.getFeetPos()) == ours, "resolver installed");
        }
    }
    // Hand-checked values for a player looking east, clicking a west face.
    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    auto const* chest = withState(*lo::game::blockWith("minecraft:chest"), "minecraft:cardinal_direction", "north");
    ctx.require(chest != nullptr, "chest state");
    auto const* c = bp::placementState(p, region, t, *chest, kWest, p.getFeetPos());
    ctx.check(c && stateText(*c, "minecraft:cardinal_direction") == "west", "a chest faces the player (west)");
    auto const* log = withState(*lo::game::blockWith("minecraft:oak_log"), "pillar_axis", "y");
    ctx.require(log != nullptr, "log state");
    auto const* l = bp::placementState(p, region, t, *log, kWest, p.getFeetPos());
    ctx.check(l && stateText(*l, "pillar_axis") == "x", "a log clicked on a west face lies along x");
});

LO_REGISTER_SELFTEST("blockplace.keeps_states", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(220));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    // Slab half is not a direction: a top slab stays a top slab.
    auto const* slab = lo::game::blockWith("minecraft:oak_slab");
    ctx.require(slab != nullptr, "oak_slab exists");
    auto const* top = withState(*slab, "minecraft:vertical_half", "top");
    ctx.require(top != nullptr, "top slab state");
    auto const* s = bp::placementState(p, region, t, *top, kUp, p.getFeetPos());
    ctx.check(s && stateText(*s, "minecraft:vertical_half") == "top", "slab half kept");
    // Rails keep their shape.
    auto const* rail = withState(*lo::game::blockWith("minecraft:rail"), "rail_direction", "1");
    ctx.require(rail != nullptr, "rail state");
    auto const* r = bp::placementState(p, region, t, *rail, kUp, p.getFeetPos());
    ctx.check(!r || stateText(*r, "rail_direction") == "1", "rail_direction kept");
    // Exception: the trapdoor half follows the placement (feet height => bottom half on a top face).
    auto const* trap = withState(*lo::game::blockWith("minecraft:trapdoor"), "upside_down_bit", "true");
    ctx.require(trap != nullptr, "trapdoor state");
    auto const* tr = bp::placementState(p, region, t, *trap, kUp, p.getFeetPos());
    ctx.check(tr && stateText(*tr, "upside_down_bit") == "false", "upside_down_bit recomputed (exception)");
    // [QUIRK] The half follows the player's height, not where the face is clicked: from the same
    // level a stair clicked on a side is a bottom stair, from one block above an upside-down one.
    auto const* stairs = withState(*lo::game::blockWith("minecraft:oak_stairs"), "upside_down_bit", "false");
    ctx.require(stairs != nullptr, "stairs state");
    auto const* low = bp::placementState(p, region, t, *stairs, kWest, p.getFeetPos());
    ctx.check(low && stateText(*low, "upside_down_bit") == "false", "stairs from the same level: bottom half");
    ctx.require(ctx.setBlock(gp(add(t, -2, 0, 0)), "minecraft:stone"), "step");
    standAt(p, add(t, -2, 1, 0), kEast);
    co_await ctx.ticks(2);
    auto const* high = bp::placementState(p, region, t, *stairs, kWest, p.getFeetPos());
    ctx.check(high && stateText(*high, "upside_down_bit") == "true", "stairs from above: upside down");
    standAt(p, add(t, -2, 0, 0), kEast);
    ctx.setBlock(gp(add(t, -2, 0, 0)), "minecraft:air");
    co_await ctx.ticks(2);
    // Floor torch clicked on the side of a block: Java would switch blocks (wall torch), so the stored
    // state is kept; a wall torch keeps being a wall torch.
    ctx.require(ctx.setBlock(gp(add(t, 1, 0, 0)), "minecraft:stone"), "torch support");
    auto const* torch = withState(*lo::game::blockWith("minecraft:torch"), "torch_facing_direction", "top");
    ctx.require(torch != nullptr, "torch state");
    auto const* tt = bp::placementState(p, region, t, *torch, kWest, p.getFeetPos());
    ctx.check(!tt || stateText(*tt, "torch_facing_direction") == "top", "a floor torch stays a floor torch");
    auto const* wall = withState(*torch, "torch_facing_direction", "north");
    ctx.require(wall != nullptr, "wall torch state");
    auto const* wt = bp::placementState(p, region, t, *wall, kWest, p.getFeetPos());
    ctx.check(
        wt && stateText(*wt, "torch_facing_direction") != "top",
        "a wall torch stays on a wall (" + (wt ? stateText(*wt, "torch_facing_direction") : std::string("none")) + ")"
    );
});

LO_REGISTER_SELFTEST("blockplace.place_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(230));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto&    p      = ctx.player();
    auto&    region = regionOf(ctx);
    BlockPos src    = add(t, 0, 0, 2); // outside the area: the chest to carry
    ctx.require(ctx.setBlock(gp(add(src, 0, -1, 0)), "minecraft:stone") && ctx.setBlock(gp(src), "minecraft:chest"), "source chest");
    co_await ctx.ticks(1);
    auto box = region.tryGetContainer(src);
    ctx.require(box.has_value(), "source container");
    box->setItem(0, lo::game::makeItem("minecraft:diamond", 9));
    box->setItem(13, lo::game::makeItem("minecraft:apple", 4));
    auto cap = carry::captureBlock(region, src);
    ctx.require(cap && cap->hasBlockEntity(), "captured");
    carry::removeCarriedBlock(region, src);

    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    CarryData d;
    d.type        = CarryType::Block;
    d.block       = cap->block;
    d.blockEntity = cap->blockEntity;
    carry::set(p, d);
    carry::applySlowness(p, carry::blockAmplifier(d));
    ctx.check(bp::tryPlace(p, add(t, 0, -1, 0), kUp) == PlaceResult::TickGuard, "no placement in the pickup tick");
    co_await ctx.ticks(1);

    // The real path: right-click the floor (use-block, deferred one tick).
    rightClick(p, add(t, 0, -1, 0), Facing::Up, Vec3{0.5f, 1.0f, 0.5f});
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    ctx.require(region.getBlock(t).getTypeName() == "minecraft:chest", "the chest is placed on the floor");
    ctx.check(stateText(region.getBlock(t), "minecraft:cardinal_direction") == "west", "the chest faces the player");
    auto placed = region.tryGetContainer(t);
    ctx.require(placed.has_value(), "placed container");
    ctx.check(placed->getItem(0).mCount == 9 && placed->getItem(0).getTypeName() == "minecraft:diamond", "slot 0 restored");
    ctx.check(placed->getItem(13).mCount == 4 && placed->getItem(13).getTypeName() == "minecraft:apple", "slot 13 restored");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness removed");
    placed->removeAllItems();
});

LO_REGISTER_SELFTEST("blockplace.double_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(240));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    // A single chest facing west at t; the carried chest goes south of it, facing west too.
    auto const* west = withState(*lo::game::blockWith("minecraft:chest"), "minecraft:cardinal_direction", "west");
    ctx.require(setState(ctx, t, west), "existing chest");
    standAt(p, add(t, -2, 0, 1), kEast);
    co_await ctx.ticks(2);
    std::string be = R"({id:"Chest",Items:[],Findable:0b,isMovable:1b})";
    auto        d  = blockCarry(*lo::game::blockWith("minecraft:chest"), be);
    carry::set(p, d);
    co_await ctx.ticks(1);
    BlockPos target = add(t, 0, 0, 1);
    ctx.require(bp::tryPlace(p, add(target, 0, -1, 0), kUp) == PlaceResult::Placed, "placed");
    co_await ctx.ticks(2);
    ctx.check(stateText(region.getBlock(target), "minecraft:cardinal_direction") == "west", "same facing");
    auto* a = chestAt(region, t);
    auto* b = chestAt(region, target);
    ctx.require(a && b, "both are chests");
    ctx.check(b->mLargeChestPaired != nullptr && a->mLargeChestPaired != nullptr, "the chests are paired");
    if (auto big = region.tryGetContainer(target)) ctx.check(big->getContainerSize() == 54, "one large chest");
});

LO_REGISTER_SELFTEST("blockplace.failures", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(250));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    standAt(p, t, kEast);
    co_await ctx.ticks(2);
    auto d = blockCarry(*lo::game::blockWith("minecraft:stone"));
    carry::set(p, d);
    carry::applySlowness(p, 0);
    co_await ctx.ticks(1);

    // The player stands in the target.
    ctx.check(bp::tryPlace(p, add(t, 0, -1, 0), kUp) == PlaceResult::CannotPlace, "obstructed by the player");
    // Clicking a stone side whose neighbour is stone too: not replaceable.
    ctx.require(ctx.setBlock(gp(add(t, 1, 0, 0)), "minecraft:stone") && ctx.setBlock(gp(add(t, 2, 0, 0)), "minecraft:stone"), "stones");
    ctx.check(bp::tryPlace(p, add(t, 1, 0, 0), 5) == PlaceResult::CannotPlace, "non-replaceable target");
    ctx.check(region.getBlock(add(t, 2, 0, 0)).getTypeName() == "minecraft:stone", "the clicked neighbour is unchanged");
    ctx.check(carry::isCarrying(p) && carry::get(p).isBlock(), "a failure keeps the carry");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == 0, "a failure keeps Slowness");

    // Protection: a cancelled PlayerPlacingBlockEvent refuses silently.
    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    bool seen     = false;
    auto listener = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerPlacingBlockEvent>(
        [&seen](ll::event::PlayerPlacingBlockEvent& ev) {
            if (!carry::inProtectionCheck()) return;
            seen = true;
            ev.cancel();
        }
    );
    auto r = bp::tryPlace(p, add(t, 0, -1, 0), kUp);
    ll::event::EventBus::getInstance().removeListener(listener);
    ctx.check(seen, "the placing event is published inside the protection check");
    ctx.check(r == PlaceResult::Protected, "a protection veto refuses");
    ctx.check(region.getBlock(t).isAir() && carry::isCarrying(p), "nothing placed, carry kept");

    // Above the height range: refused silently after the protection stage.
    int      top   = region.getMaxHeight() - 1;
    BlockPos ceil  = BlockPos{t.x, top, t.z};
    ctx.require(ctx.setBlock(gp(ceil), "minecraft:stone"), "block at the top of the world");
    ctx.check(bp::tryPlace(p, ceil, kUp) == PlaceResult::OutsideWorld, "outside the height range");
    ctx.check(carry::isCarrying(p), "carry kept");

    // Finally it works.
    ctx.check(bp::tryPlace(p, add(t, 0, -1, 0), kUp) == PlaceResult::Placed, "placed");
    ctx.check(region.getBlock(t).getTypeName() == "minecraft:stone", "stone placed");
    ctx.check(!carry::isCarrying(p), "carry cleared");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness removed");
});

LO_REGISTER_SELFTEST("blockplace.water", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(260));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    // A chest put into a water source keeps the water; stone does not.
    ctx.require(ctx.setBlock(gp(t), "minecraft:water"), "water");
    carry::set(p, blockCarry(*lo::game::blockWith("minecraft:chest"), R"({id:"Chest",Items:[]})"));
    co_await ctx.ticks(1);
    ctx.require(bp::tryPlace(p, t, kUp) == PlaceResult::Placed, "chest placed into water");
    ctx.check(region.getBlock(t).getTypeName() == "minecraft:chest", "chest there");
    ctx.check(region.getExtraBlock(t).getTypeName() == "minecraft:water", "the chest is waterlogged");
    region.setExtraBlock(t, *lo::game::blockWith("minecraft:air"), 3);

    BlockPos t2 = add(t, 0, 0, 1);
    ctx.require(ctx.setBlock(gp(t2), "minecraft:water"), "water 2");
    carry::set(p, blockCarry(*lo::game::blockWith("minecraft:stone")));
    co_await ctx.ticks(1);
    ctx.require(bp::tryPlace(p, t2, kUp) == PlaceResult::Placed, "stone placed into water");
    ctx.check(region.getBlock(t2).getTypeName() == "minecraft:stone", "stone there");
    ctx.check(region.getExtraBlock(t2).getTypeName() != "minecraft:water", "no water with stone");
});

LO_REGISTER_SELFTEST("blockplace.skull_rotation", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(270));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    auto  rotationAt = [&](BlockPos const& pos) -> std::optional<float> {
        auto cap = carry::captureBlock(region, pos);
        if (!cap || !cap->hasBlockEntity()) return std::nullopt;
        auto tag = carry::parseNbt(cap->blockEntity);
        if (!tag || !tag->contains("Rotation", Tag::Type::Float)) return std::nullopt;
        return tag->at("Rotation").get<FloatTag>().data;
    };
    for (float yaw : {kSouth, kEast, 33.0f, 150.0f, -120.0f}) {
        standAt(p, add(t, -2, 0, 0), yaw);
        co_await ctx.ticks(2);
        ctx.setBlock(gp(t), "minecraft:air");
        auto item = lo::game::makeItem("minecraft:skeleton_skull");
        useItemOn(p, item, add(t, 0, -1, 0), Facing::Up, Vec3{0.5f, 1.0f, 0.5f});
        co_await ctx.ticks(1);
        auto vanilla = rotationAt(t);
        if (!vanilla) {
            ctx.check(false, "vanilla placed a skull with a rotation (got " + region.getBlock(t).getTypeName() + ")");
            continue;
        }
        auto cap = carry::captureBlock(region, t);
        ctx.require(cap.has_value(), "captured");
        carry::removeCarriedBlock(region, t);
        CarryData d;
        d.type        = CarryType::Block;
        d.block       = cap->block;
        d.blockEntity = cap->blockEntity;
        // Carried with another rotation.
        if (auto tag = carry::parseNbt(d.blockEntity)) {
            (*tag)["Rotation"] = *vanilla + 90.0f;
            d.blockEntity      = carry::toSnbt(*tag);
        }
        carry::set(p, d);
        co_await ctx.ticks(1);
        ctx.require(bp::tryPlace(p, add(t, 0, -1, 0), kUp) == PlaceResult::Placed, "skull placed");
        auto ours = rotationAt(t);
        ctx.require(ours.has_value(), "placed skull rotation");
        float diff = std::fmod(std::fabs(*ours - *vanilla), 360.0f);
        ctx.check(
            diff < 0.5f || diff > 359.5f,
            "yaw " + std::to_string(yaw) + ": rotation " + std::to_string(*ours) + " (vanilla " + std::to_string(*vanilla) + ")"
        );
    }
});

LO_REGISTER_SELFTEST("blockplace.use_denied", [](Context& ctx) -> ll::coro::CoroTask<> {
    BlockPos t = lo::game::blockPos(ctx.testPos(280));
    ctx.require(co_await ctx.ensureLoaded(gp(t)), "test area loaded");
    buildArea(ctx, t);
    auto& p      = ctx.player();
    auto& region = regionOf(ctx);
    standAt(p, add(t, -2, 0, 0), kEast);
    co_await ctx.ticks(2);
    ctx.require(ctx.setBlock(gp(t), "minecraft:trapdoor"), "trapdoor");
    co_await ctx.ticks(1);
    // Control: without a carry the click opens the trapdoor.
    rightClick(p, t, Facing::Up, Vec3{0.5f, 1.0f, 0.5f});
    co_await ctx.ticks(2);
    std::string afterControl = stateText(region.getBlock(t), "open_bit");
    ctx.info("trapdoor open_bit after a plain click: " + afterControl);
    // Carrying: the click is cancelled (the trapdoor stays as it is) and the block goes on top.
    carry::set(p, blockCarry(*lo::game::blockWith("minecraft:stone")));
    co_await ctx.ticks(1);
    rightClick(p, t, Facing::Up, Vec3{0.5f, 1.0f, 0.5f});
    co_await ctx.ticks(3);
    ctx.check(stateText(region.getBlock(t), "open_bit") == afterControl, "the clicked trapdoor is not used");
    ctx.check(region.getBlock(add(t, 0, 1, 0)).getTypeName() == "minecraft:stone", "the block is placed on top");
    ctx.cleanupBlock(gp(add(t, 0, 1, 0)));
    ctx.check(!carry::isCarrying(p), "the carry is cleared");
    ctx.check(afterControl == "1" || afterControl == "true", "a plain click does open the trapdoor (the test sees use-block)");
});
