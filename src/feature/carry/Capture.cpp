#include "feature/carry/Capture.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "ll/api/service/Bedrock.h"

#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/entity/utilities/leashable_utility/LeashableUtility.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/ChestBlockActor.h"
#include "mc/world/phys/AABB.h"

#include "feature/carry/CarryData.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lo::feature::carry {

namespace {
constexpr int kUpdateAll = 3;

std::string stringTag(CompoundTag const& tag, std::string_view name) {
    auto const* t = tag.get(name);
    if (!t || t->getId() != Tag::Type::String) return {};
    return static_cast<std::string const&>(*static_cast<StringTag const*>(t));
}

bool isWater(Block const& block) {
    auto const& id = block.getTypeName();
    return id == "minecraft:water" || id == "minecraft:flowing_water";
}

AABB blockBox(BlockPos const& pos) {
    return AABB{
        Vec3{static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z)},
        Vec3{static_cast<float>(pos.x + 1), static_cast<float>(pos.y + 1), static_cast<float>(pos.z + 1)},
    };
}

std::unordered_set<std::int64_t> itemsAround(BlockSource& region, BlockPos const& pos) {
    std::unordered_set<std::int64_t> ids;
    for (auto* a : region.getEntities(blockBox(pos), 1.0f, ActorType::ItemEntity)) {
        if (a) ids.insert(a->getOrCreateUniqueID().rawID);
    }
    return ids;
}

ChestBlockActor* asChest(BlockActor* actor) {
    return actor && actor->getType() == BlockActorType::Chest ? static_cast<ChestBlockActor*>(actor) : nullptr;
}

void pairWithNeighbour(BlockSource& region, BlockPos const& pos, ChestBlockActor& chest) {
    if (chest.mLargeChestPaired) return;
    for (auto const& [dx, dz] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
        BlockPos npos{pos.x + dx, pos.y, pos.z + dz};
        auto*    other = asChest(region.getBlockEntity(npos));
        if (!other || other->mLargeChestPaired) continue;
        if (region.getBlock(npos).getTypeName() != region.getBlock(pos).getTypeName()) continue;
        if (!chest.canPairWith(other, region)) continue;
        chest._pairWith(other, region, true);
        other->_pairWith(&chest, region, false);
        other->refresh(region);
        return;
    }
}
} // namespace

std::string toSnbt(CompoundTag const& tag) { return tag.toSnbt(SnbtFormat::Minimize, 0); }

std::optional<CompoundTag> parseNbt(std::string_view snbt) {
    if (snbt.empty()) return std::nullopt;
    auto r = CompoundTag::fromSnbt(snbt);
    if (!r) return std::nullopt;
    return std::move(*r);
}

std::optional<CapturedBlock> captureBlock(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return std::nullopt;
    CapturedBlock out;
    out.block = toSnbt(*region.getBlock(pos).mSerializationId);
    if (auto* actor = region.getBlockEntity(pos)) {
        CompoundTag tag;
        auto        ctx = SaveContextFactory::createCloneSaveContext();
        if (actor->save(tag, *ctx)) out.blockEntity = toSnbt(tag);
    }
    return out;
}

bool removeCarriedBlock(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return false;
    auto before = itemsAround(region, pos);
    if (auto* chest = asChest(region.getBlockEntity(pos)); chest && chest->mLargeChestPaired) {
        auto* other = chest->mLargeChestPaired;
        chest->unpair(region);
        if (other) other->refresh(region);
    }
    if (auto container = region.tryGetContainer(pos)) container->removeAllItems();

    Block const* replacement = game::blockWith("minecraft:air");
    auto const&  extra       = region.getExtraBlock(pos);
    if (isWater(extra)) {
        replacement = &extra; // registry block: stays valid
        if (auto const* air = game::blockWith("minecraft:air")) region.setExtraBlock(pos, *air, kUpdateAll);
    }
    if (!replacement) return false;
    bool ok = game::setBlock(region, pos, *replacement);

    for (auto* a : region.getEntities(blockBox(pos), 1.0f, ActorType::ItemEntity)) {
        if (a && !before.contains(a->getOrCreateUniqueID().rawID)) a->remove();
    }
    return ok;
}

Block const* carriedBlock(CarryData const& data) {
    if (!data.isBlock()) return nullptr;
    auto tag = parseNbt(data.block);
    if (!tag) return nullptr;
    auto block = Block::tryGetFromRegistry(*tag);
    return block ? block.as_ptr() : nullptr;
}

std::string carriedBlockId(CarryData const& data) {
    if (!data.isBlock()) return {};
    auto tag = parseNbt(data.block);
    return tag ? stringTag(*tag, "name") : std::string{};
}

bool restoreBlock(BlockSource& region, BlockPos const& pos, CarryData const& data, Block const* state, bool waterlog) {
    Block const* block = state ? state : carriedBlock(data);
    if (!block) return false;
    std::shared_ptr<BlockActor> actor;
    if (!data.blockEntity.empty()) {
        if (auto tag = parseNbt(data.blockEntity)) {
            (*tag)["x"] = pos.x;
            (*tag)["y"] = pos.y;
            (*tag)["z"] = pos.z;
            for (auto key : {"pairlead", "pairx", "pairz", "forceunpair"}) tag->erase(key);
            if (auto level = ll::service::getLevel()) {
                DefaultDataLoadHelper helper;
                actor = BlockActor::loadStatic(block->getBlockType(), pos, *level, *tag, helper);
            }
        }
        if (!actor) {
            // Never place the block without its contents.
            log().warn("carry: could not rebuild the block actor of {} at {} {} {}", block->getTypeName(), pos.x, pos.y, pos.z);
            return false;
        }
    }
    BlockChangeContext ctx{};
    bool ok = actor ? region.setBlock(pos, *block, kUpdateAll, actor, nullptr, ctx) : game::setBlock(region, pos, *block);
    if (!ok) return false;
    if (waterlog) {
        if (auto const* water = game::blockWith("minecraft:water")) region.setExtraBlock(pos, *water, kUpdateAll);
    }
    if (auto* placed = region.getBlockEntity(pos)) {
        if (auto* chest = asChest(placed)) pairWithNeighbour(region, pos, *chest);
        placed->refresh(region);
    }
    return true;
}

std::string captureActor(Actor const& actor) {
    CompoundTag tag;
    actor.save(tag);
    return toSnbt(tag);
}

void removeCapturedActor(Actor& actor) {
    if (actor.isLeashed()) LeashableUtility::unleash(actor, LeashableUtility::UnleashReason::Broken);
    actor.removeAllPassengers(false, false);
    if (actor.getVehicle()) actor.stopRiding(true, false, false, false);
    actor.remove();
}

Actor* spawnActor(BlockSource& region, Vec3 const& pos, std::string_view snbt) {
    auto tag = parseNbt(snbt);
    if (!tag) return nullptr;
    (*tag)["Pos"] = ListTag{pos.x, pos.y, pos.z};
    auto level    = ll::service::getLevel();
    if (!level) return nullptr;
    // Loaded with the default helper, which keeps the UniqueID (BlockSource::spawnActor gives a new one).
    DefaultDataLoadHelper helper;
    auto ctx = level->getActorFactory().loadActor(&*tag, helper, region.getDimension().mHeightRange, nullptr);
    if (!ctx) return nullptr;
    return level->addEntity(region, std::move(ctx));
}

std::string entityIdOf(std::string_view snbt) {
    auto tag = parseNbt(snbt);
    return tag ? stringTag(*tag, "identifier") : std::string{};
}

} // namespace lo::feature::carry
