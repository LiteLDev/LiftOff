#pragma once

// What a player carries (Carry On's CarryOnData). Plain data: no game pointers, safe to keep across
// ticks, serialised to JSON for the world DB and the network.
//
// NBT is kept as SNBT text (LeviLamina `CompoundTag::toSnbt(SnbtFormat::Minimize)` /
// `CompoundTag::fromSnbt`): typed (1b, 2s, 3L, 4f, 5d, [B;..], [I;..], [L;..]) and lossless; the
// `carry.data` selftest checks the round trip of real block-actor and entity NBT. Turn it back into a
// tag with `Capture.h` (`parseNbt`).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace lo::feature::carry {

enum class CarryType : std::uint8_t {
    Invalid = 0, ///< not carrying
    Block,
    Entity,
    Player,
};

[[nodiscard]] std::string_view carryTypeName(CarryType type); ///< "invalid", "block", "entity", "player"
[[nodiscard]] CarryType        carryTypeFromName(std::string_view name);

struct CarryData {
    CarryType type = CarryType::Invalid;

    /// Block: SNBT of the block state, `{name:"minecraft:chest",states:{...},version:...}`.
    std::string block;
    /// Block: SNBT of the block actor (`BlockActor::save`, with id and x/y/z of the pickup position),
    /// "" when the block has none.
    std::string blockEntity;
    /// Entity: SNBT of the whole actor (`Actor::save`, incl. identifier and UniqueID).
    std::string entity;
    /// Player: the carried player's UUID (`mce::UUID::asString()`).
    std::string player;

    /// The carry key state (Carry On keeps it in the record). Not persisted: false after login.
    bool keyPressed = false;
    /// The active script's id ("" = none), set by the scripts unit (U8) at pickup.
    std::string activeScript;
    /// The active script's `render` object (opaque; U8 writes it, U12 reads it on clients).
    nlohmann::json scriptRender;

    /// Hotbar slot locked while carrying (set by `CarryStore::set`).
    int selected = 0;
    /// The carrier's tick count at the last change (`CarryStore::set`), for the tick guard.
    std::int64_t tick = -1;

    [[nodiscard]] bool isCarrying() const { return type != CarryType::Invalid; }
    [[nodiscard]] bool isBlock() const { return type == CarryType::Block; }
    [[nodiscard]] bool isEntity() const { return type == CarryType::Entity; }
    [[nodiscard]] bool isPlayer() const { return type == CarryType::Player; }

    /// Resets the carried object (type, NBT, player, script) but keeps keyPressed / selected / tick
    /// (Carry On's `clear()`).
    void clear();

    /// Length of the record as SNBT text (`{type:"block",keyPressed:0b,block:{..},tile:{..},...}`),
    /// the measure Carry On uses for the block Slowness (`nbtSize() / 500`).
    [[nodiscard]] std::size_t nbtSize() const;

    [[nodiscard]] nlohmann::json toJson() const;
    /// Unknown / missing fields take their defaults; never throws.
    [[nodiscard]] static CarryData fromJson(nlohmann::json const& j);

    friend bool operator==(CarryData const&, CarryData const&) = default;
};

} // namespace lo::feature::carry
