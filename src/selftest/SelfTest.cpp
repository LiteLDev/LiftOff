#include "selftest/SelfTest.h"

#include <algorithm>

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

#include "game/Blocks.h"
#include "mod/Log.h"
#include "core/Glob.h"

namespace lo::selftest {

namespace {
bool gRunning       = false;
int  gPlayerCounter = 0;

BlockSource* overworld();

/// The world spawn with a usable y: Bedrock keeps y = 32767 ("find the surface") until the spawn
/// is resolved, so the y is taken from the surface of the spawn column when out of range.
BlockPos spawnSurface() {
    auto level = ll::service::getLevel();
    if (!level) return {};
    BlockPos spawn = level->getSharedSpawnPos();
    auto*    region = overworld();
    if (region && (spawn.y < region->getMinHeight() || spawn.y >= region->getMaxHeight())) {
        spawn.y = region->hasChunksAt(spawn, 0, false) ? region->getAboveTopSolidBlock(spawn.x, spawn.z, false, false) : -60;
    }
    return spawn;
}

BlockSource* overworld() {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dim = level->getDimension(0).lock();
    if (!dim) return nullptr;
    return &dim->getBlockSourceFromMainChunkSource();
}
} // namespace

Context::Context(std::string name) : mName(std::move(name)) {}

Context::~Context() {
    for (auto* p : mPlayers) {
        if (!p) continue;
        p->simulateDisconnect();
    }
    if (auto* region = overworld()) {
        for (auto const& pos : mBlocks) game::clearBlock(*region, game::blockPos(pos));
    }
}

SimulatedPlayer& Context::spawnPlayer(std::string const& name) {
    auto level = ll::service::getLevel();
    if (!level) fail("no level loaded");
    auto spawn = spawnSurface();
    auto pos   = Vec3{static_cast<float>(spawn.x) + 0.5f, static_cast<float>(spawn.y) + 1.0f, static_cast<float>(spawn.z) + 0.5f};
    auto n     = name.empty() ? "lo_test_" + std::to_string(++gPlayerCounter) : name;
    auto sp    = SimulatedPlayer::create(n, pos, 0);
    if (!sp) fail("could not spawn a simulated player");
    mPlayers.push_back(sp.as_ptr());
    return *sp;
}

Player& Context::player() {
    if (mPlayers.empty()) return spawnPlayer();
    return *mPlayers.front();
}

core::GlobalPos Context::testPos(int dx, int dz) const {
    auto level = ll::service::getLevel();
    if (!level) return {};
    auto spawn = spawnSurface();
    // Units own dx ranges of 100 (U<n>: 100n..100n+99). Spread them over a compact area that the
    // run keeps loaded (see `loadTestArea`): x = 5 + dx % 100 east of the spawn (so a larger dx is
    // still farther from a player at the spawn), one 5-block row in z per hundred.
    int col = ((dx % 100) + 100) % 100;
    int row = (dx - col) / 100;
    return {0, spawn.x + 5 + col, spawn.y, spawn.z + dz + (row - 9) * 5};
}

bool Context::setBlock(core::GlobalPos const& pos, std::string const& id, std::vector<std::pair<std::string, int>> const& states) {
    auto* region = overworld();
    if (!region) return false;
    std::vector<std::pair<std::string_view, int>> s;
    for (auto const& [k, v] : states) s.emplace_back(k, v);
    auto const* block = game::blockWith(id, s);
    if (!block) return false;
    mBlocks.push_back(pos);
    return game::setBlock(*region, game::blockPos(pos), *block);
}

ll::coro::CoroTask<bool> Context::ensureLoaded(core::GlobalPos pos) {
    auto& p = player();
    p.teleport(
        Vec3{static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y) + 2.f, static_cast<float>(pos.z) + 0.5f},
        DimensionType(pos.dim)
    );
    for (int i = 0; i < 100; ++i) {
        auto level = ll::service::getLevel();
        if (auto dim = level ? level->getDimension(pos.dim).lock() : nullptr) {
            if (dim->getBlockSourceFromMainChunkSource().hasChunksAt(game::blockPos(pos), 2, false)) co_return true;
        }
        co_await ll::chrono::ticks(1);
    }
    co_return false;
}

void Context::cleanupBlock(core::GlobalPos const& pos) { mBlocks.push_back(pos); }

int Context::give(Player& player, ItemStack const& item, int slot) {
    auto& inv = player.getInventory();
    if (slot < 0) {
        for (int i = 0; i < inv.getContainerSize(); ++i) {
            if (inv.getItem(i).isNull()) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0 || slot >= inv.getContainerSize()) return -1;
    inv.setItem(slot, item);
    player.sendInventory(false);
    return slot;
}

ll::coro::CoroTask<> Context::ticks(int n) { co_await ll::chrono::ticks(std::max(n, 1)); }

void Context::check(bool condition, std::string const& message) {
    if (!condition) mFailures.push_back(message);
}

void Context::require(bool condition, std::string const& message) {
    if (!condition) fail(message);
}

void Context::fail(std::string const& message) {
    mFailures.push_back(message);
    throw Abort(message);
}

void Context::info(std::string const& message) const { log().info("SELFTEST INFO {}: {}", mName, message); }

bool running() { return gRunning; }

namespace {
constexpr char const* kTestArea = "lo_selftest";

void runServerCommand(std::string const& command) {
    auto level = ll::service::getLevel();
    if (!level) return;
    ServerCommandOrigin origin("liftoff-selftest", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
    ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin);
}

/// Keeps the test area (`Context::testPos`: x spawn..+110, z spawn-60..+60) loaded for the whole
/// run, so tests never race the chunk loading of freshly spawned players.
ll::coro::CoroTask<> loadTestArea() {
    auto level = ll::service::getLevel();
    if (!level) co_return;
    BlockPos spawn = level->getSharedSpawnPos();
    spawn.y        = 0; // y is irrelevant for the ticking area (it spans the whole height)
    auto at = [&](int dx, int dz) { return std::to_string(spawn.x + dx) + " 0 " + std::to_string(spawn.z + dz); };
    runServerCommand("tickingarea add " + at(-16, -64) + " " + at(120, 64) + " " + kTestArea + " true");
    for (int i = 0; i < 200; ++i) {
        auto dim    = level->getDimension(0).lock();
        bool loaded = dim != nullptr;
        for (int dx = -16; loaded && dx <= 112; dx += 16) {
            for (int dz = -64; loaded && dz <= 64; dz += 16) {
                loaded = dim->getBlockSourceFromMainChunkSource().hasChunksAt(BlockPos{spawn.x + dx, spawn.y, spawn.z + dz}, 0, false);
            }
        }
        if (loaded) co_return;
        co_await ll::chrono::ticks(1);
    }
    log().warn("selftest: the test area around the spawn did not load in time");
}
} // namespace

void runAll(std::string const& glob, std::function<void(std::string const&)> out) {
    if (gRunning) {
        out("SELFTEST BUSY");
        return;
    }
    std::vector<TestEntry const*> tests;
    for (auto const& t : TestRegistry::entries()) {
        if (glob.empty() || core::globMatch(glob, t.name)) tests.push_back(&t);
    }
    std::sort(tests.begin(), tests.end(), [](auto* a, auto* b) { return a->name < b->name; });
    gRunning  = true;
    auto emit = [out](std::string const& line) {
        log().info("{}", line);
        if (out) out(line);
    };
    emit("SELFTEST START count=" + std::to_string(tests.size()));
    ll::coro::keepThis([tests = std::move(tests), emit]() -> ll::coro::CoroTask<> {
        int pass = 0, fail = 0;
        co_await loadTestArea();
        for (auto const* t : tests) {
            std::string reason;
            {
                Context ctx(t->name);
                auto    result = co_await t->run(ctx).tryGet();
                if (!ctx.failures().empty()) {
                    for (auto const& f : ctx.failures()) reason += (reason.empty() ? "" : "; ") + f;
                } else if (!result) {
                    reason = "exception: " + result.error().message();
                }
            } // cleanup (players, blocks) before the next test
            if (reason.empty()) {
                ++pass;
                emit("SELFTEST PASS " + t->name);
            } else {
                ++fail;
                emit("SELFTEST FAIL " + t->name + ": " + reason);
            }
            co_await ll::chrono::ticks(1);
        }
        runServerCommand(std::string("tickingarea remove ") + kTestArea);
        emit("SELFTEST DONE pass=" + std::to_string(pass) + " fail=" + std::to_string(fail));
        gRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

} // namespace lo::selftest
