#ifdef LO_CLIENT

// Carry On's `start_riding` / `start_riding_other` on modded clients (docs/SPEC.md §3.3). The game
// already links the riders through its own SetActorLinkPacket; these packets only make sure the
// client's view agrees (Carry On's client mounts / dismounts the named entities itself), so they
// act only when the client still disagrees.

#include <cstdint>

#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/LocalPlayer.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::client::playercarry {

namespace {

Actor* entityById(Actor& anyActor, std::int64_t id) {
    return anyActor.getLevel().fetchEntity(ActorUniqueID{id}, false);
}

/// `rider` rides / leaves `mount` on this client unless it already does.
void applyRide(Actor& mount, Actor& rider, bool ride) {
    if (ride) {
        if (!rider.isRiding(&mount)) rider.startRiding(mount, /*force*/ true);
    } else if (rider.isRiding(&mount)) {
        rider.stopRiding(true, false, false, false);
    }
}

} // namespace

class PlayerCarryClientModule {
public:
    void install() {
        if (mRegistered) return;
        mRegistered = true;
        // The local player is the mount (the carrier): `entity` rides / leaves it.
        lo::client::carry::onStartRiding([](std::int64_t entity, bool ride) {
            auto* self = currentPlayer();
            if (!self) return;
            Actor& me = *self;
            if (auto* other = entityById(me, entity)) applyRide(me, *other, ride);
        });
        lo::client::carry::onStartRidingOther([](std::int64_t mountId, std::int64_t riderId, bool ride) {
            auto* self = currentPlayer();
            if (!self) return;
            Actor& me    = *self;
            auto*  mount = entityById(me, mountId);
            auto*  rider = entityById(me, riderId);
            if (mount && rider) applyRide(*mount, *rider, ride);
        });
    }
    void uninstall() {}

private:
    bool mRegistered = false;
};

} // namespace lo::client::playercarry

LO_REGISTER_MODULE(lo::client::playercarry::PlayerCarryClientModule);

#endif // LO_CLIENT
