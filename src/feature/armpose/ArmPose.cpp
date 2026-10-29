// U13 armpose: server side (see ArmPose.h).

#include "feature/armpose/ArmPose.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/network/Packet.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "feature/armpose/StandIn.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::feature::armpose {

namespace ap = core::armpose;
using carry::CarryData;
using carry::CarryVisual;

// ---- pose ---------------------------------------------------------------------------------------

namespace {
constexpr float kPlayerWidth = 0.6f;

ap::ArmOverride overrideOf(std::array<std::optional<float>, 3> const& rot, bool render) {
    ap::ArmOverride o;
    o.rotation = rot;
    o.render   = render;
    return o;
}

float blockShapeWidth(std::string const& blockSnbt) {
    CarryData tmp;
    tmp.type          = carry::CarryType::Block;
    tmp.block         = blockSnbt;
    auto const* block = carry::carriedBlock(tmp);
    if (!block) return 1.0f;
    AABB        buf;
    auto const& shape = block->getBlockType().getVisualShape(*block, buf);
    float       w     = shape.max.x - shape.min.x;
    return w > 0.0f ? w : 1.0f;
}
} // namespace

float renderWidth(CarryData const& data, CarryVisual const& visual, bool sideways, float entityWidth, Level* level) {
    using Kind = CarryVisual::Kind;
    if (data.isPlayer()) return ap::entityRenderWidth(kPlayerWidth, sideways);
    switch (visual.kind) {
    case Kind::Item:
        return ap::blockRenderWidth(1.0f, true);
    case Kind::Block:
        return ap::blockRenderWidth(blockShapeWidth(visual.block), visual.itemOverride);
    case Kind::Entity: {
        float w = entityWidth >= 0.0f ? entityWidth : entityBox(visual.entity, level).width;
        return ap::entityRenderWidth(w, sideways);
    }
    case Kind::None:
        break;
    }
    return 1.0f;
}

ap::Pose poseFor(CarryData const& data, bool renderArms, bool sideways, Level* level) {
    if (!data.isCarrying()) return {};
    auto visual = carry::visualFor(data);
    return ap::computePose(
        data.isEntity() || data.isPlayer(),
        renderWidth(data, visual, sideways, -1.0f, level),
        overrideOf(visual.rightArm, visual.renderRightArm),
        overrideOf(visual.leftArm, visual.renderLeftArm),
        renderArms
    );
}

// ---- viewers ------------------------------------------------------------------------------------

namespace {
constexpr int kJoinDelay     = 40; ///< ticks a joining client gets before it is sent anything
constexpr int kResendDelay   = 2;  ///< ticks after the carrier was (re)announced to a client
constexpr int kMinViewChunks = 4;

std::int64_t idOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

struct Carrier {
    CarryData                      data;
    int                            version = 0;
    std::vector<ap::SlotAnimation> anims;
    StandIn                        standIn;
    std::optional<Placement>       placed; ///< last placement sent
    std::int64_t                   resendAt = -1;
};

struct Pair {
    int           version = -1;
    int           dim     = 0;
    std::uint64_t standIn = 0;     ///< id shown, 0 = none
    bool          modded  = false; ///< the viewer runs the client mod (no stand-in)
};

struct PairKey {
    std::int64_t carrier, viewer;
    friend bool  operator==(PairKey const&, PairKey const&) = default;
};
struct PairKeyHash {
    std::size_t operator()(PairKey const& k) const {
        return std::hash<std::int64_t>{}(k.carrier) * 31u ^ std::hash<std::int64_t>{}(k.viewer);
    }
};

struct State {
    std::unordered_map<std::int64_t, Carrier>                 carriers;
    std::unordered_map<PairKey, Pair, PairKeyHash>            pairs;
    std::unordered_set<std::int64_t>                          dirty; ///< carriers whose carry changed
    std::unordered_set<std::int64_t>                          announced;
    std::unordered_map<std::int64_t, std::vector<SentPacket>> testViewers;
    std::int64_t                                              tick      = 0;
    bool                                                      installed = false;
};
State& state() {
    static State s;
    return s;
}

/// A viewer that gets the arm pose: every client (simulated players only as test viewers). Modded
/// clients get the same animations (they load them into their animation group, see
/// src/client/armpose); only vanilla clients get stand-ins.
bool isViewer(Player const& p) {
    if (p.isSimulatedPlayer()) return state().testViewers.contains(idOf(p));
    return true;
}

void send(Player const& viewer, std::shared_ptr<Packet> const& pkt, SentPacket record) {
    if (!pkt) return;
    auto& s = state();
    if (auto it = s.testViewers.find(idOf(viewer)); it != s.testViewers.end()) {
        it->second.push_back(std::move(record));
        return;
    }
    pkt->sendTo(viewer);
}

void sendAnimations(Player const& viewer, Player const& carrier, std::vector<ap::SlotAnimation> const& anims) {
    for (auto const& a : anims) {
        send(
            viewer,
            animatePacket(carrier, a.animation, a.controller),
            {"animate", a.animation, {{"controller", a.controller}}}
        );
    }
}

void sendRemove(Player const& viewer, std::uint64_t id) {
    send(viewer, removePacket(id), {"remove", "", {{"id", id}}});
}

void sendStandIn(Player const& viewer, StandIn const& s, Placement const& at) {
    moveAddPacket(s, at);
    send(
        viewer,
        s.add,
        {
            "add",
            s.actorType,
            {{"id", s.id}, {"scale", s.scale}, {"block", s.item}}
    }
    );
    if (s.equip) send(viewer, s.equip, {"equip", s.item, {{"id", s.id}}});
}

/// Ends everything a viewer was shown for a carrier (no carry any more, out of view, viewer turned modded).
void endPair(Player const* viewer, Player const* carrier, Pair const& pair) {
    if (!viewer) return;
    if (pair.standIn) sendRemove(*viewer, pair.standIn);
    if (carrier) sendAnimations(*viewer, *carrier, ap::vanillaAnimations(ap::Pose{}));
}

/// Rebuilds a carrier's pose and stand-in after its carry changed.
void refresh(Carrier& c, CarryData const& data) {
    c.data = data;
    c.version++;
    // Vanilla clients have no client config: arms on (server default), BDS defaults otherwise.
    auto const& client = config::get<carry::CarryClientConfig>().settings;
    c.standIn          = buildStandIn(data, carry::visualFor(data), nextStandInId());
    c.anims            = ap::vanillaAnimations(poseFor(data, true, client.rotateEntitiesSideways));
    c.placed.reset();
}
bool inView(Player const& viewer, Player const& carrier) {
    if (&viewer == &carrier) return true;
    if (viewer.getDimensionId() != carrier.getDimensionId()) return false;
    auto  a      = viewer.getPosition();
    auto  b      = carrier.getPosition();
    float dx     = a.x - b.x;
    float dz     = a.z - b.z;
    int   chunks = std::max<int>(static_cast<int>(viewer.mChunkRadius), kMinViewChunks);
    float r      = static_cast<float>(chunks * 16);
    return dx * dx + dz * dz <= r * r;
}

void tick() {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto& s = state();
    ++s.tick;

    std::unordered_map<std::int64_t, Player*> players;
    std::vector<Player*>                      viewers;
    level->forEachPlayer([&](Player& p) {
        players[idOf(p)] = &p;
        bool settled     = carry::tickCount(p) >= kJoinDelay || s.testViewers.contains(idOf(p));
        if (isViewer(p) && settled) viewers.push_back(&p);
        return true;
    });
    auto playerOf = [&](std::int64_t id) -> Player* {
        auto it = players.find(id);
        return it == players.end() ? nullptr : it->second;
    };

    // Carriers: new / changed / gone.
    std::unordered_set<std::int64_t> carrying;
    carry::forEachCarrier([&](Player& p, CarryData const& data) {
        auto id = idOf(p);
        carrying.insert(id);
        auto [it, fresh] = s.carriers.try_emplace(id);
        // Changes come through onCarryChanged (key presses do not count); a fresh entry covers a carry
        // loaded at login.
        if (fresh || s.dirty.contains(id)) refresh(it->second, data);
        if (s.announced.contains(id)) it->second.resendAt = s.tick + kResendDelay;
    });
    s.dirty.clear();
    s.announced.clear();

    // Pairs whose carrier stopped carrying / left, or whose viewer left / changed its kind.
    std::erase_if(s.pairs, [&](auto const& kv) {
        auto const& [key, pair] = kv;
        Player* viewer          = playerOf(key.viewer);
        Player* carrier         = playerOf(key.carrier);
        bool    keep =
            carrying.contains(key.carrier) && viewer && isViewer(*viewer) && pair.modded == net::isModded(*viewer);
        if (!keep) endPair(viewer, carrier, pair);
        return !keep;
    });
    std::erase_if(s.carriers, [&](auto const& kv) { return !carrying.contains(kv.first); });

    for (auto& [cid, c] : s.carriers) {
        Player* carrier = playerOf(cid);
        if (!carrier) continue;
        bool                     resend = c.resendAt == s.tick;
        std::optional<Placement> at;
        if (c.standIn.valid()) at = placementFor(c.standIn, *carrier);
        bool moved = at && at != c.placed;

        for (Player* viewer : viewers) {
            PairKey key{cid, idOf(*viewer)};
            bool    self = viewer == carrier;
            if (!inView(*viewer, *carrier)) {
                if (auto it = s.pairs.find(key); it != s.pairs.end()) {
                    endPair(viewer, carrier, it->second); // the client may still track the carrier
                    s.pairs.erase(it);
                }
                continue;
            }
            auto& pair = s.pairs[key];
            int   dim  = static_cast<int>(viewer->getDimensionId());
            if (pair.version >= 0 && pair.dim != dim) pair = Pair{}; // the client dropped every actor
            pair.dim = dim;
            if (pair.version < 0) pair.modded = net::isModded(*viewer);
            if (pair.version != c.version || resend) {
                sendAnimations(*viewer, *carrier, c.anims);
                // A (re)announced carrier may have lost its stand-in on that client too: show it again.
                if (!self && !pair.modded) {
                    if (pair.standIn) sendRemove(*viewer, pair.standIn);
                    pair.standIn = 0;
                    if (at) {
                        sendStandIn(*viewer, c.standIn, *at);
                        pair.standIn = c.standIn.id;
                    }
                }
                pair.version = c.version;
            } else if (moved && pair.standIn) {
                send(*viewer, movePacket(c.standIn, *at), {"move", "", {{"id", c.standIn.id}}});
            }
        }
        if (at) c.placed = at;
    }
}
} // namespace

// ---- hook: the carrier is (re)announced to a client --------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ArmPoseAddPlayerHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::$tryCreateAddActorPacket,
    std::unique_ptr<Packet>
) {
    auto pkt = origin();
    if (state().installed) state().announced.insert(idOf(*this));
    return pkt;
}

// ---- test seams ---------------------------------------------------------------------------------

void setTestViewer(Player const& viewer, bool on) {
    auto& s = state();
    if (on) s.testViewers.try_emplace(idOf(viewer));
    else s.testViewers.erase(idOf(viewer));
}

std::vector<SentPacket> takeTestPackets(Player const& viewer) {
    auto& s  = state();
    auto  it = s.testViewers.find(idOf(viewer));
    if (it == s.testViewers.end()) return {};
    return std::exchange(it->second, {});
}

std::uint64_t standInShownTo(Player const& carrier, Player const& viewer) {
    auto& s  = state();
    auto  it = s.pairs.find(PairKey{idOf(carrier), idOf(viewer)});
    return it == s.pairs.end() ? 0 : it->second.standIn;
}

// ---- module -------------------------------------------------------------------------------------

class ArmPoseModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); });
        if (!mRegistered) {
            mRegistered = true;
            carry::onCarryChanged([](Player& p, CarryData const&, CarryData const&) {
                if (state().installed) state().dirty.insert(idOf(p));
            });
            game::db::onWorldClosed([] {
                auto& s = state();
                s.carriers.clear();
                s.pairs.clear();
                s.dirty.clear();
                s.announced.clear();
                s.testViewers.clear();
            });
        }
        state().installed = true;
        ArmPoseAddPlayerHook::hook();
    }
    void uninstall() {
        ArmPoseAddPlayerHook::unhook();
        if (mListener) ll::event::EventBus::getInstance().removeListener(mListener);
        mListener = nullptr;
        auto& s   = state();
        // Take back what clients were shown (arm poses, stand-ins).
        if (auto level = ll::service::getLevel()) {
            std::unordered_map<std::int64_t, Player*> players;
            level->forEachPlayer([&](Player& p) {
                players[idOf(p)] = &p;
                return true;
            });
            auto find = [&](std::int64_t id) -> Player* {
                auto it = players.find(id);
                return it == players.end() ? nullptr : it->second;
            };
            for (auto const& [key, pair] : s.pairs) endPair(find(key.viewer), find(key.carrier), pair);
        }
        s.carriers.clear();
        s.pairs.clear();
        s.dirty.clear();
        s.announced.clear();
        s.testViewers.clear();
        s.installed = false;
    }

private:
    ll::event::ListenerPtr mListener;
    bool                   mRegistered = false;
};

} // namespace lo::feature::armpose

LO_REGISTER_MODULE(lo::feature::armpose::ArmPoseModule);
