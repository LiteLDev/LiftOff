// Riding for entity stacking (docs/SPEC.md §3.8 step 8): a real Bedrock link when the vehicle has
// seats, otherwise an emulated link kept by this file (Java lets any entity carry one passenger).
//
// Emulated links are stored by unique id, persisted in the world DB (`lo:entityplace:rides`) and
// applied every server tick: the rider is put on top of its vehicle (feet at the vehicle's height,
// Java's default passenger attachment), without velocity or fall distance. A link ends when the
// vehicle is gone (dead, removed, picked up) while the rider is loaded, the rider is gone, either is
// picked up, they are in different dimensions, or the rider was moved away (teleport).

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/entity/components_json_legacy/RideableComponent.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Slowness.h"
#include "feature/entityplace/EntityPlace.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::entityplace {

namespace {
constexpr char const* kDbKey = "lo:entityplace:rides";
/// A rider farther than this from its seat was moved away (teleport, knockback by a command, ...).
constexpr float kMaxSeatDistance = 4.0f;

struct Link {
    std::int64_t rider;
    std::int64_t vehicle;
};

std::vector<Link>& links() {
    static std::vector<Link> v;
    return v;
}

void persist() {
    if (!game::db::available()) return;
    if (links().empty()) {
        game::db::erase(kDbKey);
        return;
    }
    nlohmann::json j = nlohmann::json::array();
    for (auto const& l : links()) j.push_back({{"rider", l.rider}, {"vehicle", l.vehicle}});
    game::db::save(kDbKey, j);
}

void loadLinks() {
    links().clear();
    auto j = game::db::load(kDbKey);
    if (!j || !j->is_array()) return;
    for (auto const& e : *j) {
        if (!e.is_object()) continue;
        auto r = e.value("rider", std::int64_t{0});
        auto v = e.value("vehicle", std::int64_t{0});
        if (r != 0 && v != 0 && r != v) links().push_back({r, v});
    }
}

std::int64_t idOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

Actor* fetch(std::int64_t id) {
    auto level = ll::service::getLevel();
    return level ? level->fetchEntity(ActorUniqueID{id}, false) : nullptr;
}

/// Removes every link whose rider or vehicle is `id`. Returns true if one was removed.
bool dropLinksOf(std::int64_t id) {
    auto& v      = links();
    auto  before = v.size();
    std::erase_if(v, [id](Link const& l) { return l.rider == id || l.vehicle == id; });
    return v.size() != before;
}

/// The UniqueID stored in entity SNBT (0 when absent).
std::int64_t uniqueIdOfSnbt(std::string const& snbt) {
    auto tag = carry::parseNbt(snbt);
    if (!tag) return 0;
    auto const* t = tag->get("UniqueID");
    if (!t || t->getId() != Tag::Type::Int64) return 0;
    return static_cast<Int64Tag const*>(t)->data;
}

Vec3 seatOf(Actor const& vehicle) {
    Vec3 feet = vehicle.getFeetPos();
    return Vec3{feet.x, feet.y + carry::entityHeight(vehicle), feet.z};
}

/// Number of emulated links below `rider` (riders of riders are positioned after their vehicles).
int depthOf(std::unordered_map<std::int64_t, std::int64_t> const& vehicleOfRider, std::int64_t rider) {
    int depth = 0;
    for (auto it = vehicleOfRider.find(rider); it != vehicleOfRider.end() && depth < 64;
         it      = vehicleOfRider.find(it->second))
        ++depth;
    return depth;
}

void tickLinks() {
    auto& v = links();
    if (v.empty()) return;
    std::unordered_map<std::int64_t, std::int64_t> vehicleOfRider;
    for (auto const& l : v) vehicleOfRider[l.rider] = l.vehicle;
    std::vector<Link> order = v;
    std::stable_sort(order.begin(), order.end(), [&](Link const& a, Link const& b) {
        return depthOf(vehicleOfRider, a.rider) < depthOf(vehicleOfRider, b.rider);
    });

    bool changed = false;
    for (auto const& l : order) {
        Actor* rider = fetch(l.rider);
        if (!rider) continue; // unloaded: keep the link until it is loaded again
        Actor* vehicle = fetch(l.vehicle);
        bool   drop    = !vehicle || !vehicle->isAlive() || !rider->isAlive()
                    || vehicle->getDimensionId() != rider->getDimensionId() || rider->getVehicle() != nullptr;
        Vec3 seat;
        if (!drop) {
            seat = seatOf(*vehicle);
            drop = rider->getPosition().distanceTo(seat) > kMaxSeatDistance;
        }
        if (drop) {
            std::erase_if(v, [&](Link const& x) { return x.rider == l.rider && x.vehicle == l.vehicle; });
            changed = true;
            continue;
        }
        rider->moveTo(seat, rider->getRotation());
        const_cast<Vec3&>(rider->getPosDelta()) = Vec3{0.f, 0.f, 0.f};
        rider->setFallDistance(0.f);
    }
    if (changed) persist();
}

void addLink(Actor const& rider, Actor const& vehicle) {
    dropLinksOf(idOf(rider));
    links().push_back({idOf(rider), idOf(vehicle)});
    persist();
}

std::int64_t emulatedVehicleId(Actor const& rider) {
    auto id = idOf(rider);
    for (auto const& l : links())
        if (l.rider == id) return l.vehicle;
    return 0;
}
} // namespace

Actor* vehicleOf(Actor const& actor) {
    if (auto* v = actor.getVehicle()) return v;
    auto id = emulatedVehicleId(actor);
    return id ? fetch(id) : nullptr;
}

Actor* firstPassengerOf(Actor const& actor) {
    if (auto* p = actor.getFirstPassenger()) return p;
    auto id = idOf(actor);
    for (auto const& l : links()) {
        if (l.vehicle != id) continue;
        if (auto* r = fetch(l.rider)) return r;
    }
    return nullptr;
}

Actor& rootVehicleOf(Actor& actor) {
    Actor* a = &actor;
    for (int guard = 0; guard < 256; ++guard) {
        Actor* v = vehicleOf(*a);
        if (!v || v == &actor) break;
        a = v;
    }
    return *a;
}

bool ride(Actor& rider, Actor& vehicle) {
    stopEmulatedRide(rider);
    // Real link when the vehicle has seats. Forced: Java lets any entity board an empty vehicle,
    // while Bedrock seats also filter by family (a pig's seat only takes players).
    if (vehicle.getEntityContext().hasComponent<RideableComponent>()) {
        rider.startRiding(vehicle, true);
        if (rider.isRiding(&vehicle)) return true;
    }
    // Players move themselves (client-authoritative movement): no emulation for them.
    if (rider.isPlayer()) return false;
    addLink(rider, vehicle);
    rider.moveTo(seatOf(vehicle), rider.getRotation());
    return true;
}

void stopEmulatedRide(Actor const& rider) {
    auto id = idOf(rider);
    auto& v = links();
    auto  n = v.size();
    std::erase_if(v, [id](Link const& l) { return l.rider == id; });
    if (v.size() != n) persist();
}

bool isEmulatedRide(Actor const& rider, Actor const& vehicle) {
    return emulatedVehicleId(rider) == idOf(vehicle);
}

namespace {
// A picked-up entity leaves every emulated link (as rider or as vehicle), so that it does not snap
// back onto its old vehicle when it is placed again.
[[maybe_unused]] bool const kListeners = [] {
    carry::onCarryChanged([](Player&, carry::CarryData const&, carry::CarryData const& now) {
        if (!now.isEntity()) return;
        if (auto id = uniqueIdOfSnbt(now.entity); id != 0 && dropLinksOf(id)) persist();
    });
    game::db::onWorldReady([] { loadLinks(); });
    game::db::onWorldClosed([] { links().clear(); });
    return true;
}();
} // namespace

/// Applies the emulated links every tick and keeps them across world loads.
class EntityRidingModule {
public:
    void install() {
        if (game::db::available()) loadLinks();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            tickLinks();
        });
    }
    void uninstall() {
        if (mTick) ll::event::EventBus::getInstance().removeListener(mTick);
        mTick = nullptr;
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lo::feature::entityplace

LO_REGISTER_MODULE(lo::feature::entityplace::EntityRidingModule);
