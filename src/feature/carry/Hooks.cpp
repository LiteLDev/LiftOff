#include "feature/carry/Hooks.h"

#include <algorithm>
#include <exception>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::carry {

namespace {
bool gInProtectionCheck = false;

PlacementStateResolver& resolver() {
    static PlacementStateResolver r;
    return r;
}

/// Registry entries sorted by priority (highest first, stable). Registries are filled at static init,
/// so the order is computed once (again only if an entry was added later).
template <class Entry>
std::vector<Entry const*> const& sortedByPriority(std::vector<Entry> const& all) {
    static std::vector<Entry const*> sorted;
    static Entry const*              first = nullptr;
    if (sorted.size() != all.size() || (!all.empty() && first != &all.front())) {
        sorted.clear();
        for (auto const& e : all) sorted.push_back(&e);
        std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->priority > b->priority; });
        first = all.empty() ? nullptr : &all.front();
    }
    return sorted;
}

std::vector<PickupHooks const*> const& sortedHooks() { return sortedByPriority(PickupHookRegistry::entries()); }

/// The per-tick stage logs only the first failure of each hook (no per-tick logging).
template <class F>
void guardedQuiet(PickupHooks const& h, F&& fn) {
    try {
        fn();
    } catch (std::exception const& e) {
        static std::vector<PickupHooks const*> reported;
        if (std::find(reported.begin(), reported.end(), &h) != reported.end()) return;
        reported.push_back(&h);
        log().error("carry hook '{}' (onLoop) failed: {} (later failures are not logged)", h.name, e.what());
    }
}

template <class F>
bool guarded(PickupHooks const& h, char const* stage, F&& fn, bool onError) {
    try {
        return fn();
    } catch (std::exception const& e) {
        log().error("carry hook '{}' ({}) failed: {}", h.name, stage, e.what());
        return onError;
    }
}

/// Publishes a synthetic protection event; true when nobody cancelled it.
template <class Event>
bool publishProtection(Event& ev) {
    gInProtectionCheck = true;
    try {
        ll::event::EventBus::getInstance().publish(ev);
    } catch (std::exception const& e) {
        log().error("carry: protection event listener failed: {}", e.what());
    }
    gInProtectionCheck = false;
    return !ev.isCancelled();
}
} // namespace

bool overrideChecks(PickupTarget const& target) {
    for (auto* h : sortedHooks()) {
        if (!h->overrideChecks) continue;
        if (guarded(*h, "overrideChecks", [&] { return h->overrideChecks(target); }, false)) return true;
    }
    return false;
}

bool customConditions(PickupTarget const& target) {
    for (auto* h : sortedHooks()) {
        if (!h->customCondition) continue;
        if (!guarded(*h, "customCondition", [&] { return h->customCondition(target); }, false)) return false;
    }
    return true;
}

bool protectionAllows(PickupTarget const& target) {
    if (target.isBlock() && target.pos) {
        ll::event::PlayerDestroyBlockEvent ev(target.player, *target.pos);
        if (!publishProtection(ev)) return false;
    }
    for (auto* h : sortedHooks()) {
        if (!h->protection) continue;
        if (!guarded(*h, "protection", [&] { return h->protection(target); }, false)) return false;
    }
    return true;
}

bool beforePickup(PickupTarget const& target, CarryData& data) {
    for (auto* h : sortedHooks()) {
        if (!h->beforePickup) continue;
        if (!guarded(*h, "beforePickup", [&] { return h->beforePickup(target, data); }, false)) return false;
    }
    return true;
}

bool placeAllowed(PlaceTarget const& target, bool block) {
    if (block) {
        auto                                face = static_cast<uchar>(target.face);
        ll::event::PlayerPlacingBlockEvent ev(target.player, target.pos, face);
        if (!publishProtection(ev)) return false;
    }
    for (auto* h : sortedHooks()) {
        if (!h->placeProtection) continue;
        if (!guarded(*h, "placeProtection", [&] { return h->placeProtection(target); }, false)) return false;
    }
    return true;
}

void runOnPlace(Player& player, CarryData const& data) {
    for (auto* h : sortedHooks()) {
        if (!h->onPlace) continue;
        guarded(
            *h,
            "onPlace",
            [&] {
                h->onPlace(player, data);
                return true;
            },
            true
        );
    }
}

bool inProtectionCheck() { return gInProtectionCheck; }

void setPlacementStateResolver(PlacementStateResolver fn) { resolver() = std::move(fn); }

Block const& resolvePlacementState(
    Player&         player,
    BlockSource&    region,
    BlockPos const& target,
    Block const&    stored,
    int             face,
    Vec3 const&     clickPos
) {
    if (auto const& r = resolver()) {
        try {
            if (auto const* b = r(player, region, target, stored, face, clickPos)) return *b;
        } catch (std::exception const& e) {
            log().error("carry: placement state resolver failed: {}", e.what());
        }
    }
    return stored;
}

CarryVisual visualFor(CarryData const& data) {
    CarryVisual v;
    if (data.isBlock()) {
        v.kind  = CarryVisual::Kind::Block;
        v.block = data.block;
    } else if (data.isEntity()) {
        v.kind   = CarryVisual::Kind::Entity;
        v.entity = data.entity;
    }
    for (auto* p : sortedByPriority(VisualProviderRegistry::entries())) {
        if (!p->apply) continue;
        try {
            p->apply(data, v);
        } catch (std::exception const& e) {
            log().error("carry visual provider '{}' failed: {}", p->name, e.what());
        }
    }
    return v;
}

std::vector<std::string> extraDebugLines(Player const& player, CarryData const& data) {
    std::vector<std::string> lines;
    for (auto const& fn : DebugLineProviderRegistry::entries()) {
        try {
            fn(player, data, lines);
        } catch (std::exception const& e) {
            log().error("carry debug line provider failed: {}", e.what());
        }
    }
    return lines;
}

/// Runs the hooks' `onLoop` every server tick for every carrier.
class CarryHooksModule {
public:
    void install() {
        bool anyLoop = std::any_of(PickupHookRegistry::entries().begin(), PickupHookRegistry::entries().end(), [](auto const& h) {
            return static_cast<bool>(h.onLoop);
        });
        if (!anyLoop) return;
        mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            auto const& hooks = sortedHooks();
            forEachCarrier([&](Player& player, CarryData const& data) {
                CarryData copy = data; // hooks may change the carry
                for (auto* h : hooks) {
                    if (h->onLoop) guardedQuiet(*h, [&] { h->onLoop(player, copy); });
                }
            });
        });
    }
    void uninstall() {
        if (mListener) ll::event::EventBus::getInstance().removeListener(mListener);
        mListener = nullptr;
    }

private:
    ll::event::ListenerPtr mListener;
};

} // namespace lo::feature::carry

LO_REGISTER_MODULE_ORDERED(lo::feature::carry::CarryHooksModule, lo::ModuleOrder::Services);
