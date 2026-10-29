// U9 clientinput, server side: key state log and the e2e carry fixture (see KeyStateLog.h).

#include "feature/clientinput/KeyStateLog.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"
#include "selftest/SelfTest.h"

namespace lo::feature::clientinput {

namespace {
std::unordered_map<std::int64_t, bool>& logged() {
    static std::unordered_map<std::int64_t, bool> m;
    return m;
}

std::int64_t idOf(Player const& p) { return p.getOrCreateUniqueID().rawID; }

void serverTick(Level& level) {
    level.forEachPlayer([](Player& p) {
        if (!net::isModded(p)) return true;
        bool pressed = carry::keyPressed(p);
        auto it      = logged().try_emplace(idOf(p), false).first;
        // A new modded player starts "up" (the server's default), so only a pressed key is news.
        if (it->second != pressed) {
            it->second = pressed;
            log().info("clientinput: {} carry key {}", p.getRealName(), pressed ? "down" : "up");
        }
        return true;
    });
}
} // namespace

std::optional<bool> loggedKeyState(Player const& player) {
    auto it = logged().find(idOf(player));
    if (it == logged().end()) return std::nullopt;
    return it->second;
}

class KeyStateLogModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent& ev) {
            serverTick(ev.level());
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
            logged().erase(idOf(ev.self()));
        }));
        if (!mHooked) {
            mHooked = true;
            game::db::onWorldClosed([] { logged().clear(); });
        }
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        logged().clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
    bool                                mHooked = false;
};

} // namespace lo::feature::clientinput

LO_REGISTER_MODULE(lo::feature::clientinput::KeyStateLogModule);

// ---- selftests (dx 900..999; no blocks are placed) -----------------------------------------------

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace ci    = lo::feature::clientinput;

// A KeyPressedPacket from a modded client reaches the store and is logged on change.
LO_REGISTER_SELFTEST("clientinput.key_arrives", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    lo::net::setTestModded(p, true);
    lo::net::injectFromClient(p, "KeyPressedPacket", {{"pressed", true}});
    ctx.check(carry::keyPressed(p), "the store has the pressed key");
    co_await ctx.ticks(2);
    ctx.check(ci::loggedKeyState(p) == true, "key down logged");
    lo::net::injectFromClient(p, "KeyPressedPacket", {{"pressed", false}});
    co_await ctx.ticks(2);
    ctx.check(!carry::keyPressed(p), "the store has the released key");
    ctx.check(ci::loggedKeyState(p) == false, "key up logged");
    // Vanilla clients (sneak = key) are not logged.
    lo::net::setTestModded(p, false);
    carry::setKeyPressed(p, true);
    co_await ctx.ticks(2);
    ctx.check(ci::loggedKeyState(p) == false, "a vanilla client's key state is not logged");
    carry::resetKeyPressed(p);
});

// E2E fixture (client scenario `clientinput`): gives / takes a carry from tagged real players.
LO_REGISTER_SELFTEST("clientinput.e2e_fixture", [](Context& ctx) {
    auto level = ll::service::getLevel();
    ctx.require(level.has_value(), "level");
    level->forEachPlayer([&](Player& p) {
        if (p.isSimulatedPlayer()) return true;
        if (p.hasTag(ci::kFixtureCarryTag)) {
            p.removeTag(ci::kFixtureCarryTag);
            auto& region = p.getDimensionBlockSource();
            auto  below  = p.getFeetBlockPos();
            below.y -= 1;
            auto cap = carry::captureBlock(region, below);
            if (!cap) {
                ctx.check(false, "could not read the block under " + p.getRealName());
                return true;
            }
            carry::CarryData d;
            d.type        = carry::CarryType::Block;
            d.block       = cap->block;
            d.blockEntity = cap->blockEntity;
            carry::set(p, std::move(d));
            auto const& now = carry::get(p);
            ctx.check(now.isCarrying(), "fixture carry set");
            lo::log().info("clientinput: e2e fixture: {} carries {} (hotbar slot {})", p.getRealName(), carry::carriedBlockId(now), now.selected);
        }
        if (p.hasTag(ci::kFixtureCheckTag)) {
            p.removeTag(ci::kFixtureCheckTag);
            auto const& data = carry::get(p);
            int         slot = p.getSelectedItemSlot();
            lo::log().info("clientinput: e2e fixture: {} is on hotbar slot {} (locked slot {})", p.getRealName(), slot, data.isCarrying() ? data.selected : -1);
            ctx.check(data.isCarrying(), p.getRealName() + " carries");
            ctx.check(slot == data.selected, p.getRealName() + " kept the locked hotbar slot " + std::to_string(data.selected) + " (is on " + std::to_string(slot) + ")");
        }
        if (p.hasTag(ci::kFixtureUncarryTag)) {
            p.removeTag(ci::kFixtureUncarryTag);
            carry::clear(p);
            lo::log().info("clientinput: e2e fixture: {} carries nothing", p.getRealName());
        }
        return true;
    });
});
} // namespace
