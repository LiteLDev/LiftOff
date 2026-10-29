// Selftests of U7 lifecycle (dx 700..799): /carryon debug | clear | place, death with and without
// keepInventory, the carried player leaving, logout / login and dimension changes.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Slowness.h"
#include "feature/lifecycle/Lifecycle.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry     = lo::feature::carry;
namespace lifecycle = lo::feature::lifecycle;
using carry::CarryData;
using carry::CarryType;

struct Result {
    bool                     ok = false;
    std::vector<std::string> lines;
    std::vector<std::string> errors;
};

Result collect(CommandOutput const& out) {
    Result r;
    r.ok = out.mSuccessCount > 0;
    for (auto const& m : out.mMessages) {
        (m.mType == CommandOutputMessageType::Error ? r.errors : r.lines).push_back(m.mMessageId);
    }
    return r;
}

/// Runs a command as the console (permission Owner).
Result asConsole(std::string const& command) {
    auto level = ll::service::getLevel();
    if (!level) return {};
    ServerCommandOrigin origin("liftoff-selftest", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
    return collect(ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin));
}

/// Runs a command as `player` (the player's own permission level).
Result asPlayer(Player& player, std::string const& command) {
    auto level = ll::service::getLevel();
    if (!level) return {};
    PlayerCommandOrigin origin(*level, player.getOrCreateUniqueID());
    return collect(ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin));
}

bool has(std::vector<std::string> const& v, std::string const& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::string joined(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

CarryData blockCarry(std::string const& id) {
    CarryData d;
    d.type  = CarryType::Block;
    d.block = carry::toSnbt(*lo::game::blockWith(id)->mSerializationId);
    return d;
}

void standAt(Player& p, BlockPos const& pos) {
    p.teleport(Vec3{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f}, p.getDimensionId());
}

/// The first `id` block in the cube of `radius` around `center` (nullopt if none).
std::optional<BlockPos> findBlock(BlockSource& region, BlockPos const& center, std::string const& id, int radius = 3) {
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx) {
                BlockPos p{center.x + dx, center.y + dy, center.z + dz};
                if (region.getBlock(p).getTypeName() == id) return p;
            }
    return std::nullopt;
}

bool syncedTo(Player& observer, Player& carrier) {
    auto msgs = lo::net::takeTestMessages(observer);
    return std::any_of(msgs.begin(), msgs.end(), [&](auto const& m) {
        return m.first.find("CarrySyncPacket") != std::string::npos
            && m.second.value("carrier", std::int64_t{0}) == carrier.getOrCreateUniqueID().rawID;
    });
}

/// Sets the keepInventory game rule for the test and restores it afterwards.
struct KeepInventory {
    bool before;
    explicit KeepInventory(bool on) : before(lifecycle::keepInventory()) { set(on); }
    ~KeepInventory() { set(before); }
    static Result set(bool on) { return asConsole(std::string("gamerule keepInventory ") + (on ? "true" : "false")); }
};
} // namespace

LO_REGISTER_SELFTEST("lifecycle.debug", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    auto none = asPlayer(p, "carryon debug");
    ctx.check(none.lines.empty() && none.errors.empty(), "no output when not carrying: " + joined(none.lines));

    auto d        = blockCarry("minecraft:barrel");
    d.blockEntity = R"({id:"Barrel",Items:[]})";
    carry::set(p, d);
    auto block = asPlayer(p, "carryon debug");
    ctx.check(has(block.lines, "Block: minecraft:barrel"), "Block line: " + joined(block.lines));
    ctx.check(has(block.lines, "BlockState: " + d.block), "BlockState line");
    ctx.check(has(block.lines, "NBT: " + d.blockEntity), "NBT line");

    CarryData pig;
    pig.type   = CarryType::Entity;
    pig.entity = R"({identifier:"minecraft:pig",CustomName:"Bob"})";
    carry::set(p, pig);
    auto named = asPlayer(p, "carryon debug");
    ctx.check(has(named.lines, "Entity: minecraft:pig"), "Entity line: " + joined(named.lines));
    ctx.check(has(named.lines, "Entity Name: Bob"), "Entity Name is the name tag");
    ctx.check(has(named.lines, "NBT: " + pig.entity), "entity NBT line");

    pig.entity  = R"({identifier:"minecraft:glow_squid"})";
    carry::set(p, pig);
    auto plain = asPlayer(p, "carryon debug");
    ctx.check(has(plain.lines, "Entity Name: Glow Squid"), "Entity Name falls back to the type name: " + joined(plain.lines));

    CarryData who;
    who.type   = CarryType::Player;
    who.player = p.getUuid().asString();
    carry::set(p, who);
    auto player = asPlayer(p, "carryon debug");
    ctx.check(player.lines.size() == 1 && player.lines[0] == "Carrying Player.", "player line: " + joined(player.lines));
    carry::clear(p);

    auto console = asConsole("carryon debug");
    ctx.check(!console.errors.empty(), "the console gets an error");
});

LO_REGISTER_SELFTEST("lifecycle.clear_self", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    carry::set(p, blockCarry("minecraft:barrel"));
    carry::applySlowness(p, 2);
    co_await ctx.ticks(1);
    auto r = asPlayer(p, "carryon clear");
    ctx.check(has(r.lines, "Cleared 1 Item!"), "message: " + joined(r.lines) + joined(r.errors));
    ctx.check(!carry::isCarrying(p), "the carry is deleted");
    ctx.check(!lo::game::db::load("lo:carry:" + p.getUuid().asString()), "the persisted record is erased");
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "Slowness is removed");
    // Nothing was placed: the barrel is not anywhere near the player.
    auto& region = p.getDimensionBlockSource();
    ctx.check(!findBlock(region, lo::game::blockPosOf(p.getFeetPos()), "minecraft:barrel", 2), "nothing is placed");

    auto again = asPlayer(p, "carryon clear");
    ctx.check(has(again.lines, "Cleared 1 Item!"), "clearing an empty carry still reports 1");

    bool admin = false;
    {
        auto level = ll::service::getLevel();
        PlayerCommandOrigin origin(*level, p.getOrCreateUniqueID());
        admin = lo::commands::isAdmin(origin);
    }
    if (!admin) {
        auto denied = asPlayer(p, "carryon place");
        ctx.check(!denied.errors.empty() && denied.lines.empty(), "place needs op");
        auto deniedOthers = asPlayer(p, "carryon clear @s");
        ctx.check(!deniedOthers.errors.empty() && deniedOthers.lines.empty(), "clear <targets> needs op");
    } else {
        ctx.info("the simulated player is an operator; permission denial not checked");
    }
});

LO_REGISTER_SELFTEST("lifecycle.admin_commands", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& a   = ctx.player();
    auto& b   = ctx.spawnPlayer();
    auto  pos = ctx.testPos(710);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(a, feet);
    co_await ctx.ticks(2);

    carry::set(a, blockCarry("minecraft:barrel"));
    auto placed = asConsole("carryon place " + a.getRealName());
    ctx.check(has(placed.lines, "Placed 1 Item!"), "place message: " + joined(placed.lines) + joined(placed.errors));
    ctx.check(!carry::isCarrying(a), "the carry is placed");
    auto& region = a.getDimensionBlockSource();
    auto  at     = findBlock(region, feet, "minecraft:barrel", 2);
    ctx.check(at.has_value(), "the barrel is next to the player");
    if (at) ctx.cleanupBlock({0, at->x, at->y, at->z});

    int players = 0;
    ll::service::getLevel()->forEachPlayer([&](Player&) {
        ++players;
        return true;
    });
    carry::set(a, blockCarry("minecraft:barrel"));
    carry::set(b, blockCarry("minecraft:barrel"));
    auto cleared = asConsole("carryon clear @a");
    auto expect  = "Cleared " + std::to_string(players) + (players == 1 ? " Item!" : " Items!");
    ctx.check(has(cleared.lines, expect), "clear @a counts every player: " + joined(cleared.lines) + " expected " + expect);
    ctx.check(!carry::isCarrying(a) && !carry::isCarrying(b), "both carries are deleted");

    auto empty = asConsole("carryon place @a[name=lo_nobody_here]");
    ctx.check(!empty.errors.empty() && empty.lines.empty(), "no targets is an error");
    auto self = asConsole("carryon clear");
    ctx.check(!self.errors.empty() && self.lines.empty(), "the console has no carry of its own (clear)");
    auto selfPlace = asConsole("carryon place");
    ctx.check(!selfPlace.errors.empty() && selfPlace.lines.empty(), "the console has no carry of its own (place)");
});

LO_REGISTER_SELFTEST("lifecycle.death_keep_inventory", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.spawnPlayer();
    auto  pos = ctx.testPos(720);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    standAt(p, lo::game::blockPos(pos));
    KeepInventory rule(true);
    co_await ctx.ticks(2);
    if (!lifecycle::keepInventory()) {
        auto r = KeepInventory::set(true);
        ctx.fail("keepInventory is on: " + joined(r.lines) + " / " + joined(r.errors));
    }
    p.setSelectedSlot(4);
    carry::set(p, blockCarry("minecraft:barrel"));
    carry::applySlowness(p, 1);
    auto uuid = p.getUuid().asString();

    asConsole("kill " + p.getRealName());
    co_await ctx.ticks(2);
    ctx.require(!p.isAlive(), "the player is dead");
    ctx.check(lifecycle::pendingDeath(uuid).has_value(), "the death is remembered");
    ctx.check(carry::isCarrying(p), "still carrying while dead");

    lo::net::setTestModded(p, true);
    (void)lo::net::takeTestMessages(p);
    ctx.require(p.simulateRespawn(), "respawned");
    co_await ctx.ticks(3);
    ctx.check(p.isAlive(), "alive again");
    ctx.check(carry::isCarrying(p) && carry::get(p).isBlock(), "the carry survives the respawn");
    ctx.check(carry::get(p).selected == 4, "the selected slot is kept");
    ctx.check(!lifecycle::pendingDeath(uuid), "the death is resolved");
    ctx.check(syncedTo(p, p), "the carry is re-synced after the respawn");
    ctx.info("Slowness after respawn: " + std::to_string(carry::slownessAmplifier(p)) + " (not re-applied)");
    lo::net::setTestModded(p, false);
    carry::clear(p);
    carry::removeSlowness(p);
});

LO_REGISTER_SELFTEST("lifecycle.death_place", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.spawnPlayer();
    auto  pos = ctx.testPos(730);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    standAt(p, feet);
    KeepInventory rule(false);
    co_await ctx.ticks(2);
    carry::set(p, blockCarry("minecraft:barrel"));
    auto uuid = p.getUuid().asString();

    asConsole("kill " + p.getRealName());
    co_await ctx.ticks(2);
    ctx.require(!p.isAlive(), "the player is dead");
    auto death = lifecycle::pendingDeath(uuid);
    ctx.require(death.has_value(), "the death is remembered");
    ctx.check(std::abs(death->x - feet.x) <= 1 && std::abs(death->z - feet.z) <= 1, "at the death position");
    ctx.check(carry::isCarrying(p), "nothing is placed before the respawn");

    ctx.require(p.simulateRespawn(), "respawned");
    co_await ctx.ticks(3);
    ctx.check(!carry::isCarrying(p), "the carry is placed at respawn");
    ctx.check(!lifecycle::pendingDeath(uuid), "the death is resolved");
    auto  level  = ll::service::getLevel();
    auto& region = level->getDimension(DimensionType(0)).lock()->getBlockSourceFromMainChunkSource();
    auto  at     = findBlock(region, feet, "minecraft:barrel", 2);
    ctx.check(at.has_value(), "the barrel is at the death position");
    if (at) ctx.cleanupBlock({0, at->x, at->y, at->z});
    co_await ctx.ticks(1);
    ctx.check(carry::slownessAmplifier(p) == -1, "no Slowness");
});

LO_REGISTER_SELFTEST("lifecycle.death_player_carry", [](Context& ctx) -> ll::coro::CoroTask<> {
    // keepInventory never keeps a carried player.
    auto& p   = ctx.spawnPlayer();
    auto& q   = ctx.spawnPlayer();
    auto  pos = ctx.testPos(740);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    standAt(p, lo::game::blockPos(pos));
    KeepInventory rule(true);
    co_await ctx.ticks(2);
    CarryData d;
    d.type   = CarryType::Player;
    d.player = q.getUuid().asString();
    carry::set(p, d);
    asConsole("kill " + p.getRealName());
    co_await ctx.ticks(2);
    ctx.require(p.simulateRespawn(), "respawned");
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p), "a carried player is released at respawn even with keepInventory");
});

LO_REGISTER_SELFTEST("lifecycle.carried_player_leaves", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& carrier = ctx.player();
    auto& victim  = ctx.spawnPlayer();
    co_await ctx.ticks(2);
    CarryData d;
    d.type   = CarryType::Player;
    d.player = victim.getUuid().asString();
    carry::set(carrier, d);
    carry::applySlowness(carrier, carry::playerAmplifier());
    asConsole("kill " + victim.getRealName());
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(carrier), "the carrier is resolved when the carried player dies");
    ctx.check(carry::slownessAmplifier(carrier) == -1, "and loses Slowness");

    auto spawn = carrier.getFeetPos();
    auto other = SimulatedPlayer::create("lo_u7_leaver", spawn, carrier.getDimensionId());
    ctx.require(other.has_value(), "second player spawned");
    co_await ctx.ticks(2);
    d.player = other->getUuid().asString();
    carry::set(carrier, d);
    other->simulateDisconnect();
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(carrier), "the carrier is resolved when the carried player leaves");
});

LO_REGISTER_SELFTEST("lifecycle.rejoin", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& observer = ctx.player();
    co_await ctx.ticks(1);
    std::string const name = "lo_u7_rejoin";
    auto first = SimulatedPlayer::create(name, observer.getFeetPos(), observer.getDimensionId());
    ctx.require(first.has_value(), "carrier spawned");
    co_await ctx.ticks(2);
    auto uuid = first->getUuid().asString();
    carry::set(*first, blockCarry("minecraft:barrel"));
    first->simulateDisconnect();
    co_await ctx.ticks(2);
    auto stored = lo::game::db::load("lo:carry:" + uuid);
    ctx.check(stored && stored->value("type", "") == "block", "the carry stays persisted after logout");

    lo::net::setTestModded(observer, true);
    (void)lo::net::takeTestMessages(observer);
    auto& again = ctx.spawnPlayer(name);
    co_await ctx.ticks(4);
    if (again.getUuid().asString() == uuid) {
        ctx.check(carry::isCarrying(again) && carry::get(again).isBlock(), "still carrying after login");
        ctx.check(syncedTo(observer, again), "modded clients get the carry after login");
        carry::clear(again);
    } else {
        // Simulated players get a fresh UUID on every spawn: the store-level path is all we can check.
        ctx.info("simulated players get a new UUID on rejoin; checked the persisted record only");
        ctx.check(!carry::isCarrying(again), "a different player carries nothing");
        lo::game::db::erase("lo:carry:" + uuid);
    }
    lo::net::setTestModded(observer, false);
});

LO_REGISTER_SELFTEST("lifecycle.dimension_change", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  pos = ctx.testPos(750);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    co_await ctx.ticks(2);
    carry::set(p, blockCarry("minecraft:barrel"));
    p.teleport(Vec3{0.5f, 130.f, 0.5f}, DimensionType(1)); // above the Nether roof: nothing to suffocate in
    bool moved = false;
    for (int i = 0; i < 400 && !moved; ++i) {
        co_await ctx.ticks(1);
        moved = static_cast<int>(p.getDimensionId()) == 1;
    }
    ctx.require(moved, "the player reached the Nether");
    co_await ctx.ticks(2);
    ctx.check(carry::isCarrying(p) && carry::get(p).isBlock(), "the carry survives the dimension change");
    carry::clear(p);
    co_await ctx.ensureLoaded(pos); // back to the overworld before the player is removed
});
