#ifdef LO_CLIENT

// Carry On's `start_riding` / `start_riding_other` on modded clients (docs/SPEC.md §3.3). On
// Bedrock the game itself links and unlinks the riders (SetActorLinkPacket, sent to every client),
// so mounting is left to it. The packets only end a ride the client still shows after the server
// ended it.

#include <cstdint>

#include "mc/client/player/LocalPlayer.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::client::playercarry {

namespace {

bool& active() {
    static bool a = false;
    return a;
}

Actor* entityById(Actor& anyActor, std::int64_t id) { return anyActor.getLevel().fetchEntity(ActorUniqueID{id}, false); }

/// `rider` leaves `mount` on this client if it still rides it.
void endRide(Actor& mount, Actor& rider) {
    if (rider.isRiding(&mount)) rider.stopRiding(true, false, false, false);
}

} // namespace

class PlayerCarryClientModule {
public:
    void install() {
        active() = true;
        static bool registered = false;
        if (registered) return;
        registered = true;
        // The local player is the mount (the carrier): `entity` got off.
        lo::client::carry::onStartRiding([](std::int64_t entity, bool ride) {
            auto* self = currentPlayer();
            if (!active() || ride || !self) return;
            Actor& me = *self;
            if (auto* other = entityById(me, entity)) endRide(me, *other);
        });
        lo::client::carry::onStartRidingOther([](std::int64_t mountId, std::int64_t riderId, bool ride) {
            auto* self = currentPlayer();
            if (!active() || ride || !self) return;
            Actor& me    = *self;
            auto*  mount = entityById(me, mountId);
            auto*  rider = entityById(me, riderId);
            if (mount && rider) endRide(*mount, *rider);
        });
    }
    void uninstall() { active() = false; }
};

} // namespace lo::client::playercarry

LO_REGISTER_MODULE(lo::client::playercarry::PlayerCarryClientModule);

#endif // LO_CLIENT
