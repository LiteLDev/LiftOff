#include "feature/carry/PlaceCarried.h"

#include <algorithm>
#include <vector>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "net/CarryProtocol.h"

namespace lo::feature::carry {

namespace {
constexpr int kSearchRadius = 14;

Vec3 toVec(BlockPos const& p) { return Vec3{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)}; }

PlaceResult placeBlock(Player& player, BlockSource& region, BlockPos const& origin, CarryData const& data) {
    PlaceResult  r;
    Block const* stored = carriedBlock(data);
    if (!stored) {
        log().warn("carry: the carried block of {} is unknown; dropping it", player.getRealName());
        return r;
    }
    Block const& state = resolvePlacementState(player, region, origin, *stored, /*down*/ 0, toVec(origin));

    std::vector<BlockPos> candidates;
    candidates.reserve((2 * kSearchRadius + 1) * (2 * kSearchRadius + 1) * (2 * kSearchRadius + 1));
    for (int dz = -kSearchRadius; dz <= kSearchRadius; ++dz)
        for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy)
            for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) candidates.emplace_back(dx, dy, dz);
    std::stable_sort(candidates.begin(), candidates.end(), [](BlockPos const& a, BlockPos const& b) {
        return a.x * a.x + a.y * a.y + a.z * a.z < b.x * b.x + b.y * b.y + b.z * b.z;
    });

    BlockPos target = origin;
    for (auto const& d : candidates) {
        BlockPos p{origin.x + d.x, origin.y + d.y, origin.z + d.z};
        if (canPlaceBlockAt(region, p, state)) {
            target = p;
            break;
        }
    }
    if (!restoreBlock(region, target, data, &state)) return r;
    r.placed = true;
    r.what   = "block " + state.getTypeName();
    r.x = target.x, r.y = target.y, r.z = target.z;
    return r;
}
} // namespace

bool isObstructed(BlockSource& region, BlockPos const& pos, Block const& block) {
    AABB shape = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
    if (shape.max.x <= shape.min.x || shape.max.y <= shape.min.y || shape.max.z <= shape.min.z) return false;
    return !region.isUnobstructedByEntities(shape, nullptr);
}

bool canPlaceBlockAt(BlockSource& region, BlockPos const& pos, Block const& block) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return false;
    if (!region.hasChunksAt(pos, 0, false)) return false;
    if (!game::replaceable(region, pos)) return false;
    if (!block.canSurvive(region, pos)) return false;
    return !isObstructed(region, pos, block);
}

PlaceResult placeCarried(Player& player) {
    return placeCarriedAt(player, player.getDimensionBlockSource(), player.getFeetPos());
}

PlaceResult placeCarriedAt(Player& player, BlockSource& region, Vec3 const& pos) {
    CarryData data = get(player);
    if (!data.isCarrying()) return {};
    PlaceResult r;
    BlockPos    origin = game::blockPosOf(pos);
    switch (data.type) {
    case CarryType::Block:
        r = placeBlock(player, region, origin, data);
        break;
    case CarryType::Entity:
        if (auto* actor = spawnActor(region, pos, data.entity)) {
            r.placed = true;
            r.what   = "entity " + actor->getTypeName();
            r.x = origin.x, r.y = origin.y, r.z = origin.z;
        } else {
            log().warn("carry: could not recreate the carried entity of {}", player.getRealName());
        }
        break;
    case CarryType::Player: {
        Player* rider = nullptr;
        if (auto level = ll::service::getLevel()) rider = level->getPlayer(mce::UUID::fromString(data.player));
        player.removeAllPassengers(false, false);
        if (rider) {
            net::broadcastStartRidingOther(player, *rider, false);
            r.what = "player " + rider->getRealName();
        } else {
            r.what = "player " + data.player;
        }
        r.placed = true;
        r.x = origin.x, r.y = origin.y, r.z = origin.z;
        break;
    }
    default:
        break;
    }
    clear(player);
    removeSlowness(player);
    return r;
}

} // namespace lo::feature::carry
