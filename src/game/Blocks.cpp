#include "game/Blocks.h"

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

namespace lo::game {

namespace {
constexpr int kUpdateAll = 3; ///< neighbours + clients
}

std::string blockId(Block const& block) { return block.getTypeName(); }

std::string blockIdAt(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return {};
    return region.getBlock(pos).getTypeName();
}

std::optional<int> intState(Block const& block, std::string_view state) {
    auto const* s = block.getBlockType().getBlockState(HashedString(std::string(state)));
    if (!s) return std::nullopt;
    return block.getState<int>(*s);
}

Block const* blockWith(std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto type = BlockType::tryGetFromRegistry(HashedString(std::string(id)));
    if (!type) return nullptr;
    Block const* block = type->mDefaultState;
    if (!block) return nullptr;
    for (auto const& [name, value] : states) {
        if (auto const* state = type->getBlockState(HashedString(std::string(name)))) {
            if (auto b = block->setState(*state, value)) block = b.as_ptr();
        }
    }
    return block;
}

bool setBlock(BlockSource& region, BlockPos const& pos, Block const& block) {
    BlockChangeContext ctx{};
    return region.setBlock(pos, block, kUpdateAll, nullptr, ctx);
}

bool clearBlock(BlockSource& region, BlockPos const& pos) {
    auto const* air = blockWith("minecraft:air");
    return air && setBlock(region, pos, *air);
}

bool replaceable(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return false;
    if (!region.hasChunksAt(pos, 0, false)) return false;
    auto const& b = region.getBlock(pos);
    return b.isAir() || b.getBlockType().canBeBuiltOver(b, region, pos);
}

int playerFacing(Actor const& actor) {
    // Bedrock yaw: 0 = south (+z), 90 = west (-x), 180/-180 = north, -90 = east; same as Java.
    float yaw = actor.getRotation().y;
    int   idx = static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3;
    return idx; // Java 2D data value: 0 S, 1 W, 2 N, 3 E
}

std::pair<int, int> facingOffset(int facing) {
    switch (facing & 3) {
    case 0:
        return {0, 1}; // south
    case 1:
        return {-1, 0}; // west
    case 2:
        return {0, -1}; // north
    default:
        return {1, 0}; // east
    }
}

core::GlobalPos globalPos(BlockSource const& region, BlockPos const& pos) {
    return {static_cast<int>(region.getDimensionId()), pos.x, pos.y, pos.z};
}

core::GlobalPos globalPos(int dim, BlockPos const& pos) { return {dim, pos.x, pos.y, pos.z}; }

BlockPos blockPos(core::GlobalPos const& pos) { return BlockPos{pos.x, pos.y, pos.z}; }

BlockPos blockPosOf(Vec3 const& pos) {
    // the same small nudge as the game's own conversion, so 63.9999 from float maths is still 64
    return BlockPos{
        static_cast<int>(std::floor(pos.x + 0.001f)),
        static_cast<int>(std::floor(pos.y + 0.001f)),
        static_cast<int>(std::floor(pos.z + 0.001f)),
    };
}

} // namespace lo::game
