#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ll/api/coro/CoroTask.h"

#include "core/GlobalPos.h"
#include "mod/Registry.h"

class ItemStack;
class Player;
class SimulatedPlayer;
class BlockPos;

namespace lo::selftest {

/// Thrown by `require()` / `fail()` to abort the current test.
struct Abort : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Helpers and assertions for one selftest run. Everything runs on the server thread.
///
/// Players: `player()` returns a SimulatedPlayer spawned for this test (created lazily near the
/// world spawn, removed afterwards). Blocks set with `setBlock()` are restored to air after the
/// test.
class Context {
public:
    explicit Context(std::string name);
    ~Context();
    Context(Context const&)            = delete;
    Context& operator=(Context const&) = delete;

    [[nodiscard]] std::string const& name() const { return mName; }

    // ---- players ------------------------------------------------------------------------------
    /// The test's simulated player (spawned on first use). Aborts the test if it can't spawn.
    [[nodiscard]] Player& player();
    /// Spawns an extra simulated player (removed after the test).
    [[nodiscard]] SimulatedPlayer& spawnPlayer(std::string const& name = {});

    // ---- world --------------------------------------------------------------------------------
    /// A free test area: a position `dx` blocks east of the world spawn at the surface (y of the
    /// spawn). Distinct tests use distinct `dx` to avoid overlaps.
    [[nodiscard]] core::GlobalPos testPos(int dx, int dz = 0) const;
    /// Sets a block by id with integer states (restored to air after the test).
    bool setBlock(core::GlobalPos const& pos, std::string const& id, std::vector<std::pair<std::string, int>> const& states = {});
    /// Brings the test player next to `pos` and waits (up to 5 s) until its chunks are loaded:
    /// `ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded")`.
    [[nodiscard]] ll::coro::CoroTask<bool> ensureLoaded(core::GlobalPos pos);
    /// Remembers a position to clear (set to air) after the test.
    void cleanupBlock(core::GlobalPos const& pos);

    // ---- items --------------------------------------------------------------------------------
    /// Puts `item` into the player's inventory slot (or the first free slot when `slot < 0`).
    /// Returns the slot used, -1 on failure.
    int give(Player& player, ItemStack const& item, int slot = -1);

    // ---- waiting ------------------------------------------------------------------------------
    /// `co_await ctx.ticks(5);` (async tests only).
    [[nodiscard]] static ll::coro::CoroTask<> ticks(int n);

    // ---- assertions ---------------------------------------------------------------------------
    /// Records a failure and continues.
    void check(bool condition, std::string const& message);
    /// Records a failure and aborts the test.
    void require(bool condition, std::string const& message);
    [[noreturn]] void fail(std::string const& message);
    /// Logs a progress line ("SELFTEST INFO <name>: ...").
    void info(std::string const& message) const;

    [[nodiscard]] std::vector<std::string> const& failures() const { return mFailures; }

private:
    std::string                  mName;
    std::vector<std::string>     mFailures;
    std::vector<SimulatedPlayer*> mPlayers;
    std::vector<core::GlobalPos> mBlocks;
};

using SyncTest  = std::function<void(Context&)>;
using AsyncTest = std::function<ll::coro::CoroTask<>(Context&)>;

struct TestEntry {
    std::string name; ///< "unit.case"
    AsyncTest   run;
};
using TestRegistry = StaticRegistry<TestEntry>;

/// Runs all tests whose name matches `glob` (`*`, `?`), one after another, printing
/// `SELFTEST PASS <name>` / `SELFTEST FAIL <name>: <reason>` and finally
/// `SELFTEST DONE pass=<N> fail=<M>`. `out` receives every line too (command output).
void runAll(std::string const& glob, std::function<void(std::string const&)> out);
[[nodiscard]] bool running();

namespace detail {
struct TestRegistrar {
    template <class F>
    TestRegistrar(std::string name, F fn) {
        if constexpr (std::is_invocable_r_v<ll::coro::CoroTask<>, F, Context&>) {
            TestRegistry::add({std::move(name), AsyncTest(std::move(fn))});
        } else {
            static_assert(std::is_invocable_v<F, Context&>, "selftest must take (lo::selftest::Context&)");
            TestRegistry::add({std::move(name), [fn = std::move(fn)](Context& ctx) -> ll::coro::CoroTask<> {
                fn(ctx);
                co_return;
            }});
        }
    }
};
} // namespace detail

} // namespace lo::selftest

/// Registers a selftest. `fn` is `void(lo::selftest::Context&)` or
/// `ll::coro::CoroTask<>(lo::selftest::Context&)` (to `co_await ctx.ticks(n)`).
///     LO_REGISTER_SELFTEST("unit.case", [](lo::selftest::Context& ctx) { ... });
#define LO_REGISTER_SELFTEST(name, ...)                                                                                \
    static ::lo::selftest::detail::TestRegistrar LO_UNIQUE(lo_selftest_registrar_)(name, __VA_ARGS__)
