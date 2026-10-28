// U7 lifecycle: death / respawn, logout / login and the carried player leaving (docs/SPEC.md §3.5).

#include "feature/lifecycle/Lifecycle.h"

#include <algorithm>
#include <cctype>
#include <functional>

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/storage/GameRule.h"
#include "mc/world/level/storage/GameRules.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/PlaceCarried.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::feature::lifecycle {

namespace {
namespace carry = lo::feature::carry;

std::string deathKey(std::string const& uuid) { return "lo:lifecycle:death:" + uuid; }

struct Death {
    int  dim = 0;
    Vec3 pos;
};

std::optional<Death> loadDeath(std::string const& uuid) {
    auto j = game::db::load(deathKey(uuid));
    if (!j || !j->is_object()) return std::nullopt;
    Death d;
    d.dim   = j->value("dim", 0);
    d.pos.x = j->value("x", 0.0f);
    d.pos.y = j->value("y", 0.0f);
    d.pos.z = j->value("z", 0.0f);
    return d;
}

/// Runs `fn` with the player `uuid` (if still online) `ticks` server ticks later.
void later(int ticks, mce::UUID uuid, std::function<void(Player&)> fn) {
    ll::coro::keepThis([ticks, uuid, fn = std::move(fn)]() -> ll::coro::CoroTask<> {
        co_await ll::chrono::ticks(ticks);
        auto level = ll::service::getLevel();
        if (!level) co_return;
        if (auto* p = level->getPlayer(uuid)) fn(*p);
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

std::string describe(carry::CarryData const& data) {
    switch (data.type) {
    case carry::CarryType::Block:
        return "block " + carry::carriedBlockId(data);
    case carry::CarryType::Entity:
        return "entity " + carry::entityIdOf(data.entity);
    case carry::CarryType::Player:
        return "player " + data.player;
    default:
        return "nothing";
    }
}

void logPlaced(Player& player, carry::PlaceResult const& r, std::string_view why) {
    if (!r.placed) return;
    log().info("carry: placed {} of {} at {} {} {} ({})", r.what, player.getRealName(), r.x, r.y, r.z, why);
}

/// Carry On: a carried player who dies or leaves resolves the carrier with the forced placement.
void releaseCarriersOf(Player& carried, std::string_view why) {
    auto uuid = carried.getUuid().asString();
    std::vector<mce::UUID> carriers;
    carry::forEachCarrier([&](Player& p, carry::CarryData const& d) {
        if (d.isPlayer() && d.player == uuid && &p != &carried) carriers.push_back(p.getUuid());
    });
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto const& id : carriers) {
        if (auto* carrier = level->getPlayer(id)) logPlaced(*carrier, carry::placeCarried(*carrier), why);
    }
}

/// After `die` (not when another hook cancelled the death): resolve the carriers of this player and
/// remember where a carrier died. `pos` / `dim` are taken before the death.
void onDeath(Player& player, Vec3 const& pos, int dim) {
    if (player.isClientSide()) return;
    auto uuid = player.getUuid().asString();
    releaseCarriersOf(player, "carried player died");
    if (!carry::isCarrying(player)) {
        game::db::erase(deathKey(uuid));
        return;
    }
    game::db::save(deathKey(uuid), {{"dim", dim}, {"x", pos.x}, {"y", pos.y}, {"z", pos.z}});
    log().info("carry: {} died carrying {}", player.getRealName(), describe(carry::get(player)));
}

/// Places the carry where the player stands once its chunk is loaded (respawn point, maybe after a
/// dimension change), retrying for up to 5 s; a carry is never dropped on the floor of the void.
void placeAtRespawnPoint(mce::UUID uuid) {
    ll::coro::keepThis([uuid]() -> ll::coro::CoroTask<> {
        for (int i = 0; i < 100; ++i) {
            co_await ll::chrono::ticks(1);
            auto level = ll::service::getLevel();
            if (!level) co_return;
            auto* p = level->getPlayer(uuid);
            if (!p) co_return; // left again: the carry stays persisted
            if (!carry::isCarrying(*p)) co_return;
            if (!p->isAlive()) continue;
            if (!p->getDimensionBlockSource().hasChunksAt(game::blockPosOf(p->getFeetPos()), 0, false)) continue;
            auto r = carry::placeCarried(*p);
            if (r.placed) {
                logPlaced(*p, r, "death");
                co_return;
            }
        }
        log().warn("carry: could not place the carry of a respawned player; it stays carried");
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

enum class AfterRespawn { Nothing, Resync, PlaceHere };

/// Java's clone at respawn, run while the player is still at the death position.
AfterRespawn beforeRespawn(Player& player) {
    if (player.isClientSide()) return AfterRespawn::Nothing;
    auto uuid  = player.getUuid().asString();
    auto death = loadDeath(uuid);
    if (!death) return AfterRespawn::Nothing;
    game::db::erase(deathKey(uuid));
    auto data = carry::get(player);
    if (!data.isCarrying()) return AfterRespawn::Nothing;
    if (keepInventory() && !data.isPlayer()) return AfterRespawn::Resync;

    auto level = ll::service::getLevel();
    auto dim   = level ? level->getDimension(DimensionType(death->dim)).lock() : nullptr;
    if (dim) {
        auto& region = dim->getBlockSourceFromMainChunkSource();
        if (region.hasChunksAt(game::blockPosOf(death->pos), 0, false)) {
            auto r = carry::placeCarriedAt(player, region, death->pos);
            if (r.placed) {
                logPlaced(player, r, "death");
                return AfterRespawn::Nothing;
            }
        }
    }
    // The death position is not loaded (e.g. the player left on the death screen and came back) or
    // nothing fits there: place it where the player respawns instead of keeping it.
    log().warn("carry: cannot place the carry of {} at the death position; placing at the respawn point", player.getRealName());
    return AfterRespawn::PlaceHere;
}

void afterRespawn(Player& player, AfterRespawn what) {
    switch (what) {
    case AfterRespawn::Resync:
        log().info("carry: {} respawned carrying {} (keepInventory)", player.getRealName(), describe(carry::get(player)));
        later(1, player.getUuid(), [](Player& p) {
            if (carry::isCarrying(p)) net::sendCarrySync(p);
        });
        break;
    case AfterRespawn::PlaceHere:
        placeAtRespawnPoint(player.getUuid());
        break;
    default:
        break;
    }
}

/// Login: the carry is persisted by UUID (CarryStore); load it, tell the modded clients about it and
/// resolve a death the player left pending on the death screen if the game respawned them already.
void onJoined(Player& player) {
    if (player.isClientSide()) return;
    if (carry::isCarrying(player)) {
        log().info("carry: {} joined carrying {}", player.getRealName(), describe(carry::get(player)));
        net::sendCarrySync(player);
    }
    if (player.isAlive() && pendingDeath(player.getUuid().asString())) afterRespawn(player, beforeRespawn(player));
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    LifecycleDieHook,
    ll::memory::HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$die,
    void,
    ActorDamageSource const& source
) {
    Vec3 pos = getFeetPos();
    int  dim = static_cast<int>(getDimensionId());
    origin(source);
    if (isAlive() && getHealth() > 0) return; // the death was cancelled
    try {
        onDeath(*this, pos, dim);
    } catch (std::exception const& e) {
        log().error("carry: death handling failed: {}", e.what());
    }
}

LL_TYPE_INSTANCE_HOOK(LifecycleRespawnHook, ll::memory::HookPriority::Normal, ServerPlayer, &ServerPlayer::$respawn, void) {
    AfterRespawn what = AfterRespawn::Nothing;
    try {
        what = beforeRespawn(*this);
    } catch (std::exception const& e) {
        log().error("carry: respawn handling failed: {}", e.what());
    }
    origin();
    afterRespawn(*this, what);
}

std::optional<core::GlobalPos> pendingDeath(std::string const& uuid) {
    auto d = loadDeath(uuid);
    if (!d) return std::nullopt;
    auto b = game::blockPosOf(d->pos);
    return core::GlobalPos{d->dim, b.x, b.y, b.z};
}

bool keepInventory() {
    auto level = ll::service::getLevel();
    if (!level) return false;
    // Looked up by name: `GameRules::getBool(GameRuleId{GameRulesIndex::KeepInventory})` reads false
    // after `/gamerule keepInventory true` on 1.26.51 (the header enum is out of step with the rules).
    for (auto const& rule : level->getGameRules().mGameRules.get()) {
        std::string const& name = rule.mName;
        if (name.size() == 13 && std::equal(name.begin(), name.end(), "keepinventory", [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            auto const* value = std::get_if<bool>(&rule.mValue.get());
            return value && *value;
        }
    }
    return false;
}

class LifecycleModule {
public:
    void install() {
        LifecycleDieHook::hook();
        LifecycleRespawnHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& ev) {
            later(1, ev.self().getUuid(), [](Player& p) { onJoined(p); });
        }));
        // Before CarryStore drops the leaving player's cache entry (High runs before Normal).
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>(
            [](ll::event::PlayerDisconnectEvent& ev) {
                Player& p = ev.self();
                releaseCarriersOf(p, "carried player left");
                // A carried player is released on logout (vanilla ejects passengers): the carrier's
                // carry ends like with Carry On's last-passenger rule. Other carries stay persisted.
                if (carry::get(p).isPlayer()) logPlaced(p, carry::placeCarried(p), "logout");
            },
            ll::event::EventPriority::High
        ));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        LifecycleRespawnHook::unhook();
        LifecycleDieHook::unhook();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::feature::lifecycle

LO_REGISTER_MODULE(lo::feature::lifecycle::LifecycleModule);
