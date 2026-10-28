#include "feature/playercarry/PlayerCarry.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/entity/components_json_legacy/RideableComponent.h"
#include "mc/entity/components_json_legacy/SeatDescription.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Feedback.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/PlaceCarried.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::feature::playercarry {

namespace carry = lo::feature::carry;
using carry::CarryData;
using carry::CarryType;

namespace {

// ---- seats ----------------------------------------------------------------------------------------

/// The fields of a seat that carrying changes.
struct SavedSeat {
    Vec3  position;
    bool  lock    = false;
    float degrees = 0.f;
};

/// Original seats of the carriers, by carrier UUID.
std::unordered_map<std::string, std::vector<SavedSeat>>& savedSeats() {
    static std::unordered_map<std::string, std::vector<SavedSeat>> m;
    return m;
}

std::string uuidOf(Player const& p) { return p.getUuid().asString(); }

RideableComponent* rideableOf(Player& p) {
    auto c = p.getEntityContext().tryGetComponent<RideableComponent>();
    return c ? c.as_ptr() : nullptr;
}

/// Height of a riding player above its seat (Bedrock: a passenger player sits 1.22 above the seat
/// point of a player vehicle, measured by `playercarry.position`).
constexpr float kRiderSeatHeight = 1.22f;

/// The seat (in the vehicle's space: x left, y up, z forward, relative to the vehicle's position)
/// that puts the riding player's feet 0.6 in front of and 0.4 above the carrier's feet.
Vec3 carrySeat() { return Vec3{0.f, kUp - kRiderSeatHeight, kForward}; }

/// Moves every seat of `carrier` to the carry seat (remembering the originals once).
void retargetSeats(Player& carrier) {
    auto* rideable = rideableOf(carrier);
    if (!rideable) return;
    auto& seats = rideable->mData->mSeats.get();
    auto  key   = uuidOf(carrier);
    if (!savedSeats().contains(key)) {
        std::vector<SavedSeat> saved;
        for (auto& s : seats) saved.push_back({s.mPosition.get(), s.mLockPassengerRotation, s.mLockPassengerRotationDegrees});
        savedSeats()[key] = std::move(saved);
    }
    for (auto& s : seats) {
        s.mPosition.get()                = carrySeat();
        s.mLockPassengerRotation         = true;
        s.mLockPassengerRotationDegrees  = kYawLimitDeg;
    }
}

/// Puts the original seats back (no-op when they were never changed).
void restoreSeats(Player& carrier) {
    auto it = savedSeats().find(uuidOf(carrier));
    if (it == savedSeats().end()) return;
    if (auto* rideable = rideableOf(carrier)) {
        auto& seats = rideable->mData->mSeats.get();
        for (size_t i = 0; i < seats.size() && i < it->second.size(); ++i) {
            seats[i].mPosition.get()               = it->second[i].position;
            seats[i].mLockPassengerRotation        = it->second[i].lock;
            seats[i].mLockPassengerRotationDegrees = it->second[i].degrees;
        }
    }
    savedSeats().erase(it);
}

Player* playerByUuid(std::string const& uuid) {
    auto level = ll::service::getLevel();
    if (!level || uuid.empty()) return nullptr;
    return level->getPlayer(mce::UUID::fromString(uuid));
}

float wrapDegrees(float d) {
    d = std::fmod(d, 360.f);
    if (d >= 180.f) d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

std::string blockText(BlockPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

} // namespace

// ---- queries ------------------------------------------------------------------------------------

Player* carriedPlayer(Player const& carrier) {
    auto const& data = carry::get(carrier);
    if (!data.isPlayer()) return nullptr;
    return playerByUuid(data.player);
}

Player* carrierOf(Player const& player) {
    Player* found = nullptr;
    auto    uuid  = uuidOf(player);
    carry::forEachCarrier([&](Player& c, CarryData const& d) {
        if (!found && d.isPlayer() && d.player == uuid) found = &c;
    });
    return found;
}

// ---- pickup (§3.2 with a player target) -----------------------------------------------------------

bool tryPickup(Player& carrier, Player& target) {
    if (&carrier == &target) return false;
    auto const& cfg = lo::config::get<carry::CarryConfig>().settings;
    // 1-2: distance, game mode, empty hands, not carrying, tick guard, key.
    if (!carry::canStartPickup(carrier, target.getFeetPos(), /*entity*/ true)) return false;
    // 3: post-damage invulnerability. 4: removed / dead.
    if (target.mInvulnerableTime > 0) return false;
    if (target.mRemoved || !target.isAlive() || target.isDead()) return false;
    // 5: players are never tamed.
    auto&               region = carrier.getDimensionBlockSource();
    carry::PickupTarget t{.player = carrier, .region = region, .entity = &target};
    // 6-7: scripts' override_checks, entity lists (a player is never a baby).
    bool override = cfg.useScripts && carry::overrideChecks(t);
    if (!carry::isEntityAllowed(target) && !override) return false;
    // 8: not in creative: size (players are never in the monster family).
    if (!carry::isCreative(carrier)) {
        float f = cfg.relativePlayerScale ? 1.f / carry::playerScale(carrier) : 1.f;
        if (carry::entityHeight(target) * f > cfg.maxEntityHeight || carry::entityWidth(target) * f > cfg.maxEntityWidth) {
            return false;
        }
    }
    // 9-10: custom pickup conditions, protection.
    if (!carry::customConditions(t)) return false;
    if (!carry::protectionAllows(t)) return false;
    // 12: the player branch.
    if (!cfg.pickupPlayers) return false;
    if (!carry::isCreative(carrier) && carry::isCreative(target)) return false;
    // 11: script conditions + activation + commandPickup.
    CarryData data;
    data.type   = CarryType::Player;
    data.player = uuidOf(target);
    if (!carry::beforePickup(t, data)) return false;

    target.removeAllPassengers(false, false);
    if (target.getVehicle()) target.stopRiding(true, false, false, false);
    // Bedrock carriers may already have shoulder parrots: they leave, so the seat is ours [LO].
    carrier.removeAllPassengers(false, false);
    retargetSeats(carrier);
    if (!target.startRiding(carrier, /*force*/ true)) {
        restoreSeats(carrier);
        log().warn("carry: {} could not mount {}", target.getRealName(), carrier.getRealName());
        return false;
    }
    net::broadcastStartRidingOther(carrier, target, true);
    carry::set(carrier, std::move(data));
    carry::swing(carrier);
    carry::playSound(carrier, carry::sound::kArmorEquip, 1.0f, 0.5f);
    carry::applySlowness(carrier, carry::playerAmplifier());
    log().info("carry: {} picked up player {}", carrier.getRealName(), target.getRealName());
    return true;
}

// ---- release ------------------------------------------------------------------------------------

void release(Player& carrier, std::string const& reason) {
    auto data = carry::get(carrier);
    if (!data.isPlayer()) return;
    Player* rider = playerByUuid(data.player);
    if (rider && rider->isRiding(&carrier)) {
        carrier.removeAllPassengers(false, false);
        net::broadcastStartRidingOther(carrier, *rider, false);
    }
    if (rider) net::sendStartRiding(carrier, *rider, false);
    carry::clear(carrier);
    carry::removeSlowness(carrier);
    restoreSeats(carrier);
    log().info(
        "carry: {} released player {} ({})",
        carrier.getRealName(),
        rider ? rider->getRealName() : data.player,
        reason
    );
}

// ---- placement (§3.7 step 3) ----------------------------------------------------------------------

bool tryPlace(Player& carrier, BlockPos const& clicked, int face) {
    auto data = carry::get(carrier);
    if (!data.isPlayer() || !carry::canAct(carrier)) return false;
    auto&    region = carrier.getDimensionBlockSource();
    BlockPos target = clicked;
    if (!game::replaceable(region, target)) {
        static constexpr int kOffsets[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        int                  f              = std::clamp(face, 0, 5);
        target = BlockPos{clicked.x + kOffsets[f][0], clicked.y + kOffsets[f][1], clicked.z + kOffsets[f][2]};
        if (!game::replaceable(region, target)) {
            carry::playSound(carrier, carry::sound::kLavaPop, 0.5f, 0.5f);
            return false;
        }
    }
    Vec3    pos{static_cast<float>(target.x) + 0.5f, static_cast<float>(target.y), static_cast<float>(target.z) + 0.5f};
    Player* rider = playerByUuid(data.player);
    carrier.removeAllPassengers(false, false);
    if (rider) net::broadcastStartRidingOther(carrier, *rider, false);
    carry::clear(carrier);
    restoreSeats(carrier);
    if (rider) rider->teleport(pos, carrier.getDimensionId());
    carry::swing(carrier);
    carry::removeSlowness(carrier);
    log().info(
        "carry: {} placed player {} at {}",
        carrier.getRealName(),
        rider ? rider->getRealName() : data.player,
        blockText(target)
    );
    log().info("carry: {} released player {} (placed)", carrier.getRealName(), rider ? rider->getRealName() : data.player);
    return true;
}

// ---- stacking (§3.8 with a carried player) ----------------------------------------------------------

bool tryStack(Player& carrier, Actor& clicked) {
    auto data = carry::get(carrier);
    if (!data.isPlayer() || !carry::canAct(carrier)) return false;
    auto const& cfg = lo::config::get<carry::CarryConfig>().settings;
    // 1: stacking enabled; held = the carrier's first passenger.
    if (!cfg.stackableEntities) return false;
    Actor* held = carrier.getFirstPassenger();
    if (!held) return false;
    // 2: the column above the clicked entity's root vehicle.
    Actor* root = &clicked;
    for (int guard = 0; root->getVehicle() && guard < 256; ++guard) root = root->getVehicle();
    int    count = 0;
    Actor* top   = root;
    for (Actor* a = root->getFirstPassenger(); a && count < 256; a = a->getFirstPassenger()) {
        ++count;
        top = a;
    }
    if (count >= cfg.maxEntityStackLimit - 1) {
        carry::playSound(carrier, carry::sound::kLavaPop, 0.5f, 0.5f);
        return true;
    }
    // 3: the held entity is already the top.
    if (top == held) return true;
    // 4: stacking lists (silent).
    if (!carry::isStackingAllowed(*top)) return true;
    // 5: size.
    if (cfg.entitySizeMattersStacking) {
        float heldArea = carry::entityHeight(*held) * carry::entityWidth(*held);
        float topArea  = carry::entityHeight(*top) * carry::entityWidth(*top);
        if (heldArea > topArea) {
            carry::playSound(carrier, carry::sound::kLavaPop, 0.5f, 0.5f);
            return true;
        }
    }
    // 6: a horse on top becomes tamed.
    if (top->getTypeName() == "minecraft:horse") {
        SynchedActorDataAccess::setActorFlag(top->getEntityContext(), ActorFlags::Tamed, true);
    }
    // 7: off the carrier and next to the clicked entity (Java spawns it 2.6 above and then moves it
    // onto the entity when near; for a player both end at the clicked entity's position).
    carrier.removeAllPassengers(false, false);
    net::broadcastStartRidingOther(carrier, *held, false);
    restoreSeats(carrier);
    auto* rider = held->isPlayer() ? static_cast<Player*>(held) : nullptr;
    if (rider) rider->teleport(clicked.getFeetPos(), clicked.getDimensionId());
    // 8: rides the top entity (not forced); a refusal leaves it standing there.
    // Java lets a player ride any entity that has room; Bedrock only entities with seats for players,
    // so a refused normal ride is forced [LO].
    bool rode = held->startRiding(*top, /*force*/ false) || held->startRiding(*top, /*force*/ true);
    // 9.
    carry::runOnPlace(carrier, data);
    carry::swing(carrier);
    carry::clear(carrier);
    carry::playSound(carrier, carry::sound::kHorseSaddle, 0.5f, 1.5f);
    carry::removeSlowness(carrier);
    log().info(
        "carry: {} stacked {} on {}{}",
        carrier.getRealName(),
        held->getTypeName(),
        top->getTypeName(),
        rode ? "" : " (not riding)"
    );
    return true;
}

// ---- tick: keep the rider in place, notice when it leaves -----------------------------------------

namespace {

void tickCarrier(Player& carrier, CarryData const& data) {
    Player* rider = playerByUuid(data.player);
    if (!rider) {
        // Carried player left the server (normally handled by the disconnect listener).
        carry::placeCarried(carrier);
        restoreSeats(carrier);
        log().info("carry: {} released player {} (left)", carrier.getRealName(), data.player);
        return;
    }
    if (!rider->isRiding(&carrier)) {
        // Dismounted (sneak), teleported, died, changed dimension: the carrier's last passenger
        // stopped riding.
        release(carrier, "dismounted");
        return;
    }
    // The seats are per entity and come back from the definition on respawn / reload.
    retargetSeats(carrier);
    // Body yaw = carrier yaw; head yaw within ±30° of it (the seat lock does it on clients).
    float yaw  = carrier.getRotation().y;
    float head = rider->getRotation().y;
    float diff = std::clamp(wrapDegrees(head - yaw), -kYawLimitDeg, kYawLimitDeg);
    if (std::abs(wrapDegrees(head - yaw)) > kYawLimitDeg) {
        // Server side only (SimulatedPlayers, other riders); a client's own view is held by the seat lock.
        const_cast<Vec2&>(rider->getRotation()).y = wrapDegrees(yaw + diff);
        rider->setYHeadRot(wrapDegrees(yaw + diff));
    }
    rider->setYBodyRotation(yaw);
}

} // namespace

// ---- module -------------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    PlayerCarryInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&      entity,
    Vec3 const& location,
    HandSlot    handSlot
) {
    Player& player = mPlayer;
    try {
        auto const& data = carry::get(player);
        if (data.isPlayer()) {
            // §3.8: stacking with the carried player; the interaction is always cancelled.
            tryStack(player, entity);
            return false;
        }
        if (!data.isCarrying() && entity.isPlayer() && tryPickup(player, static_cast<Player&>(entity))) return false;
    } catch (std::exception const& e) {
        log().error("playercarry: interact failed: {}", e.what());
    }
    return origin(entity, location, handSlot);
}

class PlayerCarryModule {
public:
    void install() {
        PlayerCarryInteractHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            std::vector<std::pair<Player*, CarryData>> carriers;
            carry::forEachCarrier([&](Player& p, CarryData const& d) {
                if (d.isPlayer()) carriers.emplace_back(&p, d);
            });
            for (auto& [p, d] : carriers) {
                try {
                    tickCarrier(*p, d);
                } catch (std::exception const& e) {
                    log().error("playercarry: tick failed: {}", e.what());
                }
            }
        }));
        // Use-block while carrying a player: put the player down (next tick: a cancelled
        // interaction rolls back world changes).
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>(
            [](ll::event::PlayerInteractBlockEvent& ev) {
                auto& player = ev.self();
                if (!carry::get(player).isPlayer()) return;
                ev.cancel();
                if (!carry::canAct(player)) return;
                auto uuid    = uuidOf(player);
                auto clicked = ev.blockPos();
                int  face    = static_cast<int>(ev.face());
                pending().push_back({uuid, clicked, face});
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            auto todo = std::exchange(pending(), {});
            for (auto& p : todo) {
                if (auto* carrier = playerByUuid(p.carrier)) tryPlace(*carrier, p.clicked, p.face);
            }
        }));
        // The carried player leaves or dies: the carrier is resolved like a forced placement.
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev
                                                                                     ) {
            auto& player = ev.self();
            if (auto* carrier = carrierOf(player)) {
                resolve(*carrier, player, "disconnected");
            }
            // A carrier leaving: the passenger is ejected by the game; release now so the seat and
            // the carry do not outlive the ride.
            if (carry::get(player).isPlayer()) release(player, "carrier left");
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDieEvent>([](ll::event::PlayerDieEvent& ev) {
            if (auto* carrier = carrierOf(ev.self())) resolve(*carrier, ev.self(), "died");
        }));
    }
    void uninstall() {
        PlayerCarryInteractHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        pending().clear();
    }

private:
    struct Pending {
        std::string carrier;
        BlockPos    clicked;
        int         face;
    };
    static std::vector<Pending>& pending() {
        static std::vector<Pending> v;
        return v;
    }
    static void resolve(Player& carrier, Player& rider, char const* why) {
        carry::placeCarried(carrier);
        restoreSeats(carrier);
        log().info("carry: {} released player {} ({})", carrier.getRealName(), rider.getRealName(), why);
    }

    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::feature::playercarry

LO_REGISTER_MODULE(lo::feature::playercarry::PlayerCarryModule);
