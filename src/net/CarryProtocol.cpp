// LiftOff client <-> server protocol (see CarryProtocol.h).

#include "net/CarryProtocol.h"

#include <exception>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/NetEventCallback.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifdef LO_CLIENT
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "client/Client.h"
#include "mc/client/player/LocalPlayer.h"
#endif

#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::net {

using Json = nlohmann::json;
using feature::carry::CarryData;

namespace {
constexpr std::uint64_t kMaxPayload = 1u << 22; ///< 4 MiB of JSON

void writeJson(BinaryStream& s, Json const& j) {
    s.writeString(j.dump(-1, ' ', false, Json::error_handler_t::replace), nullptr, nullptr);
}

Bedrock::Result<void> readJson(ReadOnlyBinaryStream& s, Json& out) {
    auto str = s.getString(kMaxPayload);
    if (!str) return nonstd::make_unexpected(str.error());
    out = Json::parse(*str, nullptr, false);
    if (out.is_discarded()) out = Json::object();
    return {};
}
} // namespace

// Packet classes need external linkage (their type name is their id).
#define LO_JSON_PACKET(Name)                                                                                           \
    class Name : public ll::network::PacketBase<Name> {                                                                \
    public:                                                                                                            \
        Json payload = Json::object();                                                                                 \
        Name()       = default;                                                                                        \
        explicit Name(Json j) : payload(std::move(j)) {}                                                               \
        void                  write(BinaryStream& s) const override { writeJson(s, payload); }                         \
        Bedrock::Result<void> read(ReadOnlyBinaryStream& s) override { return readJson(s, payload); }                  \
    };

LO_JSON_PACKET(HelloPacket)
LO_JSON_PACKET(WelcomePacket)
LO_JSON_PACKET(KeyPressedPacket)
LO_JSON_PACKET(CarrySyncPacket)
LO_JSON_PACKET(StartRidingPacket)
LO_JSON_PACKET(StartRidingOtherPacket)
#undef LO_JSON_PACKET

// ---- server side -------------------------------------------------------------------------------

namespace {
struct ServerState {
    std::unordered_set<std::int64_t>                                                  modded;
    std::unordered_map<std::int64_t, std::vector<std::pair<std::string, Json>>>       testInbox;
    std::vector<std::function<void(Player&)>>                                         joined;
};
ServerState& server() {
    static ServerState s;
    return s;
}

std::int64_t idOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

template <class P>
void sendTo(Player const& player, P const& pkt) {
    auto& s = server();
    if (!s.modded.contains(idOf(player))) return;
    if (auto it = s.testInbox.find(idOf(player)); it != s.testInbox.end()) {
        it->second.emplace_back(std::string(pkt.getName()), pkt.payload);
        return;
    }
    pkt.sendTo(player);
}

template <class P>
void sendToModded(P const& pkt) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        sendTo(p, pkt);
        return true;
    });
}

CarrySyncPacket syncPacket(Player& carrier, CarryData const& data) {
    return CarrySyncPacket(Json{
        {"carrier", idOf(carrier)                                     },
        {"runtime", static_cast<std::uint64_t>(carrier.getRuntimeID().rawID)},
        {"data",    data.toJson()                                     },
    });
}

void handleHello(Player& player, Json const& payload) {
    int  protocol = payload.value("protocol", 0);
    auto& s       = server();
    if (protocol != kProtocolVersion) {
        log().warn("{} runs LiftOff protocol {} (server: {}); treated as a vanilla client", player.getRealName(), protocol, kProtocolVersion);
        return;
    }
    bool known = !s.modded.insert(idOf(player)).second;
    sendTo(player, WelcomePacket(Json{{"protocol", kProtocolVersion}}));
    if (known) return; // a repeated hello (crossed our welcome)
    log().info("net: {} joined with the LiftOff client mod", player.getRealName());
    feature::carry::forEachCarrier([&](Player& carrier, CarryData const& data) {
        sendTo(player, syncPacket(carrier, data));
    });
    for (auto const& fn : s.joined) {
        try {
            fn(player);
        } catch (std::exception const& e) {
            log().error("net: modded-join listener failed: {}", e.what());
        }
    }
}

void handleKeyPressed(Player& player, Json const& payload) {
    if (!isModded(player)) return;
    feature::carry::setKeyPressed(player, payload.value("pressed", false));
}

void dispatchFromClient(Player& player, std::string_view name, Json const& payload) {
    try {
        if (name == "HelloPacket") handleHello(player, payload);
        else if (name == "KeyPressedPacket") handleKeyPressed(player, payload);
    } catch (std::exception const& e) {
        log().error("net: {} from {} failed: {}", name, player.getRealName(), e.what());
    }
}

ServerPlayer* senderOf(NetworkIdentifier const& id, NetEventCallback& callback, ll::network::Packet const& packet) {
    return static_cast<ServerNetworkHandler&>(callback)._getServerPlayer(id, packet.mSenderSubId);
}
} // namespace

class HelloHandler : public ll::network::PacketHandlerBase<HelloHandler, HelloPacket> {
public:
    void handlePacket(NetworkIdentifier const& id, NetEventCallback& callback, HelloPacket const& packet) const {
        if (auto* p = senderOf(id, callback, packet)) dispatchFromClient(*p, "HelloPacket", packet.payload);
    }
};

class KeyPressedHandler : public ll::network::PacketHandlerBase<KeyPressedHandler, KeyPressedPacket> {
public:
    void handlePacket(NetworkIdentifier const& id, NetEventCallback& callback, KeyPressedPacket const& packet) const {
        if (auto* p = senderOf(id, callback, packet)) dispatchFromClient(*p, "KeyPressedPacket", packet.payload);
    }
};

bool isModded(Player const& player) { return server().modded.contains(idOf(player)); }

void onModdedJoined(std::function<void(Player&)> fn) { server().joined.push_back(std::move(fn)); }

void sendCarrySync(Player& carrier) { sendToModded(syncPacket(carrier, feature::carry::get(carrier))); }

void sendCarrySyncTo(Player& receiver, Player& carrier) { sendTo(receiver, syncPacket(carrier, feature::carry::get(carrier))); }

void sendStartRiding(Player& to, Actor const& entity, bool ride) {
    sendTo(to, StartRidingPacket(Json{{"entity", idOf(entity)}, {"ride", ride}}));
}

void broadcastStartRidingOther(Actor const& mount, Actor const& rider, bool ride) {
    sendToModded(StartRidingOtherPacket(Json{{"mount", idOf(mount)}, {"rider", idOf(rider)}, {"ride", ride}}));
}

void setTestModded(Player const& player, bool modded) {
    auto& s = server();
    if (modded) {
        s.modded.insert(idOf(player));
        s.testInbox[idOf(player)];
    } else {
        s.modded.erase(idOf(player));
        s.testInbox.erase(idOf(player));
    }
}

std::vector<std::pair<std::string, Json>> takeTestMessages(Player const& player) {
    auto& s  = server();
    auto  it = s.testInbox.find(idOf(player));
    if (it == s.testInbox.end()) return {};
    return std::exchange(it->second, {});
}

void injectFromClient(Player& player, std::string const& packet, Json const& payload) {
    dispatchFromClient(player, packet, payload);
}

// ---- client side -------------------------------------------------------------------------------

#ifdef LO_CLIENT
} // namespace lo::net

namespace lo::client::carry {

using Json = nlohmann::json;
using feature::carry::CarryData;

namespace {
struct ClientState {
    std::unordered_map<std::int64_t, CarryData>                                       mirror;
    std::vector<std::function<void(std::int64_t, CarryData const&)>>                  syncListeners;
    std::vector<std::function<void(std::int64_t, bool)>>                              ridingListeners;
    std::vector<std::function<void(std::int64_t, std::int64_t, bool)>>                ridingOtherListeners;
    std::vector<std::function<void()>>                                                connectedListeners;
    bool                                                                              inLevel   = false;
    bool                                                                              welcomed  = false;
    int                                                                               helloSent = 0;
    int                                                                               ticks     = 0;
};
ClientState& state() {
    static ClientState s;
    return s;
}
constexpr int kHelloInterval = 40; ///< ticks between hello retries
constexpr int kHelloAttempts = 15;

void sendHello() {
    net::HelloPacket(Json{{"protocol", net::kProtocolVersion}}).sendToServer();
    ++state().helloSent;
}

template <class... Args, class L>
void fire(L const& listeners, char const* what, Args const&... args) {
    for (auto const& fn : listeners) {
        try {
            fn(args...);
        } catch (std::exception const& e) {
            log().error("net: client {} listener failed: {}", what, e.what());
        }
    }
}
} // namespace

CarryData const* carriedOf(std::int64_t uniqueId) {
    auto& m  = state().mirror;
    auto  it = m.find(uniqueId);
    return it == m.end() ? nullptr : &it->second;
}

CarryData const* localData() {
    auto* p = lo::client::currentPlayer();
    return p ? carriedOf(p->getOrCreateUniqueID().rawID) : nullptr;
}

bool serverHasMod() { return state().welcomed; }

void sendKeyPressed(bool pressed) {
    if (!state().welcomed) return;
    net::KeyPressedPacket(Json{{"pressed", pressed}}).sendToServer();
}

void onSync(std::function<void(std::int64_t, CarryData const&)> fn) { state().syncListeners.push_back(std::move(fn)); }
void onStartRiding(std::function<void(std::int64_t, bool)> fn) { state().ridingListeners.push_back(std::move(fn)); }
void onStartRidingOther(std::function<void(std::int64_t, std::int64_t, bool)> fn) {
    state().ridingOtherListeners.push_back(std::move(fn));
}
void onConnected(std::function<void()> fn) { state().connectedListeners.push_back(std::move(fn)); }

} // namespace lo::client::carry

namespace lo::net {

class WelcomeHandler : public ll::network::PacketHandlerBase<WelcomeHandler, WelcomePacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, WelcomePacket const& packet) const {
        auto& s = client::carry::state();
        if (s.welcomed) return;
        s.welcomed = true;
        log().info("net: the server runs LiftOff (protocol {})", packet.payload.value("protocol", 0));
        client::carry::fire(s.connectedListeners, "connected");
    }
};

class CarrySyncHandler : public ll::network::PacketHandlerBase<CarrySyncHandler, CarrySyncPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, CarrySyncPacket const& packet) const {
        auto& s       = client::carry::state();
        auto  carrier = packet.payload.value("carrier", std::int64_t{0});
        auto  data    = CarryData::fromJson(packet.payload.value("data", Json::object()));
        if (data.isCarrying()) s.mirror[carrier] = data;
        else s.mirror.erase(carrier);
        client::carry::fire(s.syncListeners, "sync", carrier, data);
    }
};

class StartRidingHandler : public ll::network::PacketHandlerBase<StartRidingHandler, StartRidingPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, StartRidingPacket const& packet) const {
        client::carry::fire(
            client::carry::state().ridingListeners,
            "start riding",
            packet.payload.value("entity", std::int64_t{0}),
            packet.payload.value("ride", false)
        );
    }
};

class StartRidingOtherHandler : public ll::network::PacketHandlerBase<StartRidingOtherHandler, StartRidingOtherPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, StartRidingOtherPacket const& packet) const {
        client::carry::fire(
            client::carry::state().ridingOtherListeners,
            "start riding other",
            packet.payload.value("mount", std::int64_t{0}),
            packet.payload.value("rider", std::int64_t{0}),
            packet.payload.value("ride", false)
        );
    }
};
#endif

// ---- module ------------------------------------------------------------------------------------

class NetModule {
public:
#ifdef LO_CLIENT
    // Client listeners at mod load, before the game's threads run.
    void load() {
        auto& bus = ll::event::EventBus::getInstance();
        mClientListeners.push_back(bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) {
            auto& s     = client::carry::state();
            s.inLevel   = true;
            s.welcomed  = false;
            s.helloSent = 0;
            s.ticks     = 0;
            s.mirror.clear();
            client::carry::sendHello();
        }));
        mClientListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s    = client::carry::state();
            s.inLevel  = false;
            s.welcomed = false;
            s.mirror.clear();
        }));
        mClientListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            auto& s = client::carry::state();
            if (!s.inLevel || s.welcomed || s.helloSent >= client::carry::kHelloAttempts) return;
            if (++s.ticks % client::carry::kHelloInterval == 0) client::carry::sendHello();
        }));
    }
#endif

    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
            auto& player = ev.self();
            // Modded players drop the carrier from their mirror (the record stays in the world DB).
            if (feature::carry::isCarrying(player)) {
                sendToModded(syncPacket(player, CarryData{}));
            }
            server().modded.erase(idOf(player));
            server().testInbox.erase(idOf(player));
        }));
        if (!mHooked) {
            mHooked = true;
            feature::carry::onCarryChanged([](Player& player, CarryData const&, CarryData const& now) {
                sendToModded(syncPacket(player, now));
            });
            game::db::onWorldClosed([] {
                server().modded.clear();
                server().testInbox.clear();
            });
        }
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
#ifdef LO_CLIENT
        for (auto& l : mClientListeners) bus.removeListener(l);
        mClientListeners.clear();
#endif
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
#ifdef LO_CLIENT
    std::vector<ll::event::ListenerPtr> mClientListeners;
#endif
    bool mHooked = false;
};

} // namespace lo::net

LO_REGISTER_MODULE_ORDERED(lo::net::NetModule, lo::ModuleOrder::Net);
