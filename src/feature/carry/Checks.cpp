#include "feature/carry/Checks.h"

#include <array>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "core/CarryMath.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Lists.h"
#include "mod/Config.h"

namespace lo::feature::carry {

GameType gameType(Player const& player) {
    auto g = player.getPlayerGameType();
    if (g == GameType::Default || g == GameType::Undefined) {
        if (auto level = ll::service::getLevel()) g = level->getDefaultGameType();
    }
    return g;
}

bool isCreative(Player const& player) { return gameType(player) == GameType::Creative; }

bool canStartPickup(Player const& player, Vec3 const& target, bool entity) {
    auto const& s     = config::get<CarryConfig>().settings;
    auto        mode  = gameType(player);
    double      scale = core::carry::reachScale(s.scaleDistance, mode == GameType::Creative, entity);
    if (!core::carry::withinReach(player.getFeetPos().distanceToSqr(target), s.maxDistance, scale)) return false;
    if (mode == GameType::Adventure || mode == GameType::Spectator) return false;
    if (!player.getSelectedItem().isNull() || !player.getOffhandSlot().isNull()) return false;
    if (isCarrying(player)) return false;
    if (!canAct(player)) return false;
    return keyPressed(player);
}

bool isDoubleHeight(Block const& block) {
    if (blockStateValue(block, "upper_block_bit")) return true;
    if (auto v = blockStateValue(block, "sea_grass_type")) return *v != "default";
    return false;
}

bool isBedrockOnlyBlockActor(std::string_view blockId) {
    static constexpr std::array<std::string_view, 7> kIds{
        "minecraft:flower_pot",
        "minecraft:noteblock",
        "minecraft:cauldron",
        "minecraft:piston",
        "minecraft:sticky_piston",
        "minecraft:lodestone",
        "minecraft:netherreactor",
    };
    for (auto id : kIds)
        if (id == blockId) return true;
    return false;
}

bool isUnbreakable(Block const& block) { return block.getBlockType().getDestroySpeed() < 0.0f; }

} // namespace lo::feature::carry
