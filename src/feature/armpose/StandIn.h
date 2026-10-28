#pragma once

// Client-only display actors that show a carried thing to vanilla clients (U13, see ArmPose.h).
// Server thread.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "mc/deps/core/math/Vec3.h"

class Level;
class Packet;
class Player;

namespace lo::feature::carry {
struct CarryData;
struct CarryVisual;
} // namespace lo::feature::carry

namespace lo::feature::armpose {

/// Collision box and scale of an entity from its NBT (a temporary actor, never added to the level).
/// Cached per NBT text. `ok` false when the NBT could not be loaded (defaults then).
struct EntityBox {
    float       width  = 0.6f;
    float       height = 1.8f;
    float       scale  = 1.0f;
    std::string type;
    bool        ok = false;
};
/// `level`: where the temporary actor is made (nullptr: the server level; modded clients pass their
/// client level, which is the only one in a remote world). Cached per thread.
[[nodiscard]] EntityBox entityBox(std::string_view snbt, Level* level = nullptr);

/// A prepared stand-in: the packets that create it on a client (sent as is to every viewer, the
/// position refreshed before each send).
struct StandIn {
    enum class Kind { None, Item, Entity };
    Kind          kind = Kind::None;
    std::uint64_t id   = 0;  ///< runtime id = unique id, never used by a server actor
    std::string   actorType; ///< "minecraft:armor_stand" for items
    std::string   item;      ///< Kind::Item: the item worn on the head
    float         scale  = 1.0f;
    float         height = 0.0f; ///< drawn height (blocks) of an entity stand-in
    float         width  = 0.0f;
    std::shared_ptr<Packet> add;   ///< AddActorPacket
    std::shared_ptr<Packet> equip; ///< MobArmorEquipmentPacket (Kind::Item)

    [[nodiscard]] bool valid() const { return kind != Kind::None && add != nullptr; }
};

/// Builds the stand-in of a carry (Kind::None for players, nothing carried or failures).
[[nodiscard]] StandIn buildStandIn(carry::CarryData const& data, carry::CarryVisual const& visual, std::uint64_t id);

/// Where the stand-in stands for `carrier` this tick (approximating Carry On's third-person transform,
/// SPEC §4.3) and its yaw (degrees).
struct Placement {
    Vec3  pos;
    float yaw = 0.0f;
    friend bool operator==(Placement const&, Placement const&) = default;
};
[[nodiscard]] Placement placementFor(StandIn const& standIn, Player const& carrier);

/// Packet factories for one stand-in.
void                                  moveAddPacket(StandIn const& standIn, Placement const& at);
[[nodiscard]] std::shared_ptr<Packet> movePacket(StandIn const& standIn, Placement const& at);
[[nodiscard]] std::shared_ptr<Packet> removePacket(std::uint64_t id);
[[nodiscard]] std::shared_ptr<Packet> animatePacket(Player const& carrier, std::string const& animation, std::string const& controller);

/// Fresh ids for stand-ins ("LO" in the high bytes; server actors count up from 1).
[[nodiscard]] std::uint64_t nextStandInId();

} // namespace lo::feature::armpose
