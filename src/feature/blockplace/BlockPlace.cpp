#include "feature/blockplace/BlockPlace.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Feedback.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/PlaceCarried.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::blockplace {

namespace carry = lo::feature::carry;

namespace {
constexpr int kUpdateAll = 3;

/// Bedrock states of Java `Direction` / `Axis` properties (SPEC §3.6).
constexpr std::array<std::string_view, 12> kDirectionStates{
    "facing_direction",
    "minecraft:facing_direction",
    "direction",
    "minecraft:cardinal_direction",
    "minecraft:block_face",
    "weirdo_direction",
    "torch_facing_direction",
    "lever_direction",
    "pillar_axis",
    "portal_axis",
    "coral_direction",
    "coral_fan_direction",
};

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

/// Skulls and heads: Java has a floor and a wall block, Bedrock one block with `facing_direction`.
bool isSkull(std::string_view id) { return id == "minecraft:skull" || endsWith(id, "_skull") || endsWith(id, "_head"); }

CompoundTag const* statesOf(CompoundTag const& serialization) {
    auto const* t = serialization.get("states");
    return t && t->getId() == Tag::Type::Compound ? static_cast<CompoundTag const*>(t) : nullptr;
}

/// Java floor/wall pairs that are one block on Bedrock (SPEC §3.6 [LO]): switching form = "different
/// block" in Java, so the stored state is kept.
bool switchesFloorWall(std::string_view id, Block const& stored, Block const& placed) {
    auto storedTorch = carry::blockStateValue(stored, "torch_facing_direction");
    auto placedTorch = carry::blockStateValue(placed, "torch_facing_direction");
    if (storedTorch && placedTorch && ((*storedTorch == "top") != (*placedTorch == "top"))) return true;
    if (isSkull(id)) {
        auto storedFacing = carry::blockStateValue(stored, "facing_direction");
        auto placedFacing = carry::blockStateValue(placed, "facing_direction");
        if (storedFacing && placedFacing && ((*storedFacing == "1") != (*placedFacing == "1"))) return true;
    }
    return false;
}

bool insideHeightRange(BlockSource& region, BlockPos const& pos) {
    return pos.y >= region.getMinHeight() && pos.y < region.getMaxHeight();
}

bool isWater(Block const& block) {
    auto const& id = block.getTypeName();
    return id == "minecraft:water" || id == "minecraft:flowing_water";
}

bool isWaterSource(Block const& block) {
    if (!isWater(block)) return false;
    auto depth = carry::blockStateValue(block, "liquid_depth");
    return !depth || *depth == "0";
}

/// Rotation of a floor head placed by a player looking along `yaw`, in 16 steps like a newly placed
/// head (Bedrock stores the placer's quantised yaw; the face then looks at the placer).
float skullRotation(float yaw) {
    int   segment = static_cast<int>(std::floor(yaw * 16.0f / 360.0f + 0.5f)) & 15;
    float deg     = static_cast<float>(segment) * 22.5f;
    return deg > 180.0f ? deg - 360.0f : deg;
}

/// Built-in block-actor rule for floor heads (SPEC §3.6): the rotation follows the placer, like a
/// newly placed head. Rewrites `data.blockEntity` before the block actor is rebuilt.
void fixSkullRotation(Player& player, Block const& state, carry::CarryData& data) {
    if (!isSkull(state.getTypeName()) || data.blockEntity.empty()) return;
    if (carry::blockStateValue(state, "facing_direction") != "1") return;
    auto tag = carry::parseNbt(data.blockEntity);
    if (!tag) return;
    (*tag)["Rotation"] = skullRotation(player.getRotation().y);
    data.blockEntity   = carry::toSnbt(*tag);
}

char const* resultName(PlaceResult r) {
    switch (r) {
    case PlaceResult::NotCarryingBlock:
        return "not carrying a block";
    case PlaceResult::TickGuard:
        return "tick guard";
    case PlaceResult::CannotPlace:
        return "cannot place";
    case PlaceResult::Protected:
        return "protected";
    case PlaceResult::OutsideWorld:
        return "outside the world";
    case PlaceResult::Failed:
        return "failed";
    case PlaceResult::Placed:
        return "placed";
    }
    return "?";
}
} // namespace

bool isRecomputedState(std::string_view blockId, std::string_view stateName) {
    for (auto s : kDirectionStates) {
        if (s == stateName) return true;
    }
    if (stateName == "hanging" && blockId == "minecraft:pointed_dripstone") return true;
    return carry::isPlacementException(stateName);
}

Block const* placementState(
    Player&         player,
    BlockSource&    region,
    BlockPos const& target,
    Block const&    stored,
    int             face,
    Vec3 const&     clickPos
) {
    (void)region;
    // Java: a miss hit at the player's position; Bedrock wants the click relative to the block.
    Vec3 rel{clickPos.x - static_cast<float>(target.x), clickPos.y - static_cast<float>(target.y), clickPos.z - static_cast<float>(target.z)};
    Block const& placed =
        stored.getBlockType().getPlacementBlock(player, target, static_cast<uchar>(face), rel, 0);
    if (&placed.getBlockType() != &stored.getBlockType()) return nullptr;
    auto const& id = stored.getTypeName();
    if (switchesFloorWall(id, stored, placed)) return nullptr;

    CompoundTag merged = *stored.mSerializationId;
    auto const* from   = statesOf(*placed.mSerializationId);
    if (!from || !statesOf(merged)) return nullptr;
    auto& into    = merged.at("states").get<CompoundTag>();
    bool  changed = false;
    for (auto const& [name, value] : from->mTags) {
        if (!into.contains(name) || !isRecomputedState(id, name)) continue;
        if (into.at(name) == value) continue;
        into.at(name) = value;
        changed       = true;
    }
    if (!changed) return &stored;
    auto block = Block::tryGetFromRegistry(merged);
    return block ? block.as_ptr() : nullptr;
}

BlockPos offset(BlockPos const& pos, int face) {
    switch (face) {
    case 0:
        return BlockPos{pos.x, pos.y - 1, pos.z};
    case 1:
        return BlockPos{pos.x, pos.y + 1, pos.z};
    case 2:
        return BlockPos{pos.x, pos.y, pos.z - 1};
    case 3:
        return BlockPos{pos.x, pos.y, pos.z + 1};
    case 4:
        return BlockPos{pos.x - 1, pos.y, pos.z};
    case 5:
        return BlockPos{pos.x + 1, pos.y, pos.z};
    default:
        return pos;
    }
}

BlockPos placementTarget(BlockSource& region, BlockPos const& clicked, int face) {
    return game::replaceable(region, clicked) ? clicked : offset(clicked, face);
}

bool hasWaterSource(BlockSource& region, BlockPos const& pos) {
    if (!insideHeightRange(region, pos) || !region.hasChunksAt(pos, 0, false)) return false;
    return isWaterSource(region.getBlock(pos)) || isWaterSource(region.getExtraBlock(pos));
}

bool canPlace(Player& player, BlockSource& region, BlockPos const& target, Block const& state, int face) {
    bool     inside = insideHeightRange(region, target);
    BlockPos column{target.x, inside ? target.y : static_cast<int>(region.getMinHeight()), target.z};
    if (!region.hasChunksAt(column, 0, false)) return false;
    // Java reads void air outside the height range: replaceable; the height check comes later.
    if (inside && !game::replaceable(region, target)) return false;
    if (inside) {
        if (!state.mayPlace(region, target, static_cast<uchar>(face), &player)) return false;
        if (!state.canSurvive(region, target)) return false;
    }
    // The player may build there: game mode, build ability, border / allow / deny blocks.
    auto mode = carry::gameType(player);
    if (mode == GameType::Adventure || mode == GameType::Spectator) return false;
    if (!player.canUseAbility(AbilitiesIndex::Build)) return false;
    if (inside && !region.checkBlockPermissions(player, target, static_cast<uchar>(face), ItemStack::EMPTY_ITEM(), false))
        return false;
    return !carry::isObstructed(region, target, state);
}

PlaceResult tryPlace(Player& player, BlockPos const& clicked, int face) {
    // 1. Carrying a block, tick guard.
    if (!carry::get(player).isBlock()) return PlaceResult::NotCarryingBlock;
    if (!carry::canAct(player)) return PlaceResult::TickGuard;
    carry::CarryData data   = carry::get(player);
    BlockSource&     region = player.getDimensionBlockSource();
    Block const*     stored = carry::carriedBlock(data);
    if (!stored) {
        log().warn("carry: the carried block of {} is unknown; it stays carried", player.getRealName());
        return PlaceResult::Failed;
    }

    // 2. Target. 3. Placement state (click = the player's feet position).
    BlockPos     target = placementTarget(region, clicked, face);
    Block const& state  = carry::resolvePlacementState(player, region, target, *stored, face, player.getFeetPos());

    // 4. Can place.
    if (!canPlace(player, region, target, state, face)) {
        carry::playSound(player, carry::sound::kLavaPop, 0.5f, 0.5f);
        return PlaceResult::CannotPlace;
    }
    // 5. Protection.
    if (!carry::placeAllowed(carry::PlaceTarget{player, region, target, data, face}, true)) return PlaceResult::Protected;
    // 6. Height range.
    if (!insideHeightRange(region, target)) return PlaceResult::OutsideWorld;
    // 7. commandPlace.
    carry::runOnPlace(player, data);
    // The script may have changed or cleared the carry: place only what was checked.
    if (auto const& now = carry::get(player); !now.isBlock() || now.tick != data.tick || now.block != data.block)
        return PlaceResult::NotCarryingBlock;

    // 8. Set the block. Water: setting a block that can hold water over water keeps the water in the
    // liquid layer (the game's own rule, = Java WATERLOGGED); only a source counts, like Java.
    bool source = hasWaterSource(region, target);
    fixSkullRotation(player, state, data);
    if (!carry::restoreBlock(region, target, data, &state, false)) {
        log().warn("carry: could not place the block of {} at {} {} {}; it stays carried", player.getRealName(), target.x, target.y, target.z);
        return PlaceResult::Failed;
    }
    if (!source && isWater(region.getExtraBlock(target))) {
        if (auto const* air = game::blockWith("minecraft:air")) region.setExtraBlock(target, *air, kUpdateAll);
    }
    region.updateNeighborsAt(BlockPos{target.x, target.y - 1, target.z});

    // 9. Clear, sound, swing, Slowness.
    std::string id = state.getTypeName();
    carry::clear(player);
    carry::playBlockSound(player, target, region.getBlock(target), carry::BlockSound::Place);
    carry::swing(player);
    carry::removeSlowness(player);
    log().info("carry: {} placed block {} at {} {} {}", player.getRealName(), id, target.x, target.y, target.z);
    return PlaceResult::Placed;
}

namespace {

/// A click waiting for the next server tick.
struct Pending {
    mce::UUID    player;
    BlockPos     clicked;
    int          face;
    int          dimension;
    std::int64_t carryTick; ///< the carry must not change in between
};

class BlockPlaceModule {
public:
    void install() {
        carry::setPlacementStateResolver(&placementState);
        auto& bus = ll::event::EventBus::getInstance();
        mInteract = bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([this](ll::event::PlayerInteractBlockEvent& ev) {
            onInteract(ev);
        });
        mTick = bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](ll::event::ServerLevelTickEvent&) { runPending(); });
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        if (mInteract) bus.removeListener(mInteract);
        if (mTick) bus.removeListener(mTick);
        mInteract = nullptr;
        mTick     = nullptr;
        mPending.clear();
        carry::setPlacementStateResolver(nullptr);
    }

private:
    void onInteract(ll::event::PlayerInteractBlockEvent& ev) {
        auto& player = ev.self();
        // Only the server's players (the client build also fires this for the local prediction).
        auto level = ll::service::getLevel();
        if (!level || reinterpret_cast<void const*>(&player.getLevel()) != reinterpret_cast<void const*>(level.as_ptr()))
            return;
        if (!carry::get(player).isBlock()) return;
        // While carrying a block the clicked block is never used, whether placement succeeds or not.
        ev.cancel();
        if (!carry::canAct(player)) return;
        // A cancelled interaction rolls back block changes: place on the next tick.
        mPending.push_back(Pending{
            player.getUuid(),
            ev.blockPos(),
            static_cast<int>(ev.face()),
            static_cast<int>(player.getDimensionId()),
            carry::get(player).tick,
        });
    }

    void runPending() {
        if (mPending.empty()) return;
        auto pending = std::move(mPending);
        mPending.clear();
        auto level = ll::service::getLevel();
        if (!level) return;
        for (auto const& p : pending) {
            Player* player = level->getPlayer(p.player);
            if (!player || static_cast<int>(player->getDimensionId()) != p.dimension) continue;
            bool unchanged = carry::get(*player).isBlock() && carry::get(*player).tick == p.carryTick;
            if (!unchanged) continue;
            auto r = tryPlace(*player, p.clicked, p.face);
            if (r != PlaceResult::Placed && r != PlaceResult::CannotPlace) {
                log().debug("carry: placement by {} not done: {}", player->getRealName(), resultName(r));
            }
        }
    }

    ll::event::ListenerPtr mInteract;
    ll::event::ListenerPtr mTick;
    std::vector<Pending>   mPending;
};

} // namespace

} // namespace lo::feature::blockplace

LO_REGISTER_MODULE(lo::feature::blockplace::BlockPlaceModule);
