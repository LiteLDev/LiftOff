#include "feature/blockpickup/BlockPickup.h"

#include <exception>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Checks.h"
#include "feature/carry/Feedback.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::blockpickup {

namespace carry = lo::feature::carry;

namespace {

/// A pickup accepted in a use-block event (steps 1-11 passed), finished on the next server tick.
struct Pending {
    mce::UUID   player;
    int         dimension = 0;
    BlockPos    pos;
    std::string block; ///< state SNBT seen at the click (the block must still be the same)
};

std::vector<Pending>& pending() {
    static std::vector<Pending> list;
    return list;
}

bool serverSide(Player const& player) { return !player.isClientSide() && !player.getLevel().isClientSide(); }

bool isPendingAt(Player const& player, BlockPos const& pos) {
    auto const& uuid = player.getUuid();
    for (auto const& p : pending())
        if (p.player == uuid && p.pos == pos) return true;
    return false;
}

/// Steps 1-10: checks without side effects. `cap` receives the block (step 3).
Refusal checks(Player& player, BlockSource& region, BlockPos const& pos, carry::CapturedBlock& cap) {
    auto const& s = config::get<carry::CarryConfig>().settings;
    // 1-2. distance (to the block centre), game mode, empty hands, not carrying, tick guard, key.
    if (!carry::canStartPickup(player, pos.center(), false)) return Refusal::Preconditions;

    // 3. block actor NBT.
    auto captured = carry::captureBlock(region, pos);
    if (!captured) return Refusal::NotLoaded;
    Block const& block = region.getBlock(pos);
    if (block.isAir()) return Refusal::NotLoaded;
    cap = std::move(*captured);

    carry::PickupTarget target{player, region, &block, &pos, &cap.blockEntity};
    // 4. script `override_checks` (asked now, in Carry On's order).
    bool override = carry::overrideChecks(target);
    // 5. black / whitelist (never overridable).
    if (!carry::isBlockAllowed(block)) return Refusal::Lists;
    // 6. double-height blocks (never overridable).
    if (carry::isDoubleHeight(block)) return Refusal::DoubleHeight;
    // 7. unbreakable blocks in survival.
    if (!override && carry::isUnbreakable(block) && !carry::isCreative(player) && !s.pickupUnbreakableBlocks)
        return Refusal::Unbreakable;
    // 8. a (Java) block entity is required unless pickupAllBlocks.
    bool hasBlockEntity = cap.hasBlockEntity() && !carry::isBedrockOnlyBlockActor(game::blockId(block));
    if (!override && !hasBlockEntity && !s.pickupAllBlocks) return Refusal::NoBlockEntity;
    // 9. locked containers.
    if (cap.hasBlockEntity() && hasLock(cap.blockEntity)) return Refusal::Locked;
    // 10. custom pickup conditions.
    if (!carry::customConditions(target)) return Refusal::CustomCondition;
    return Refusal::None;
}

/// Step 11: protection (publishes PlayerDestroyBlockEvent, then the hooks).
bool protectionAllows(Player& player, BlockSource& region, BlockPos const& pos, carry::CapturedBlock const& cap) {
    carry::PickupTarget target{player, region, &region.getBlock(pos), &pos, &cap.blockEntity};
    return carry::protectionAllows(target);
}

/// Step 12: script conditions, activation and commandPickup. Fills `data` with the block.
bool runScript(
    Player&                     player,
    BlockSource&                region,
    BlockPos const&             pos,
    carry::CapturedBlock const& cap,
    carry::CarryData&           data
) {
    data             = {};
    data.type        = carry::CarryType::Block;
    data.block       = cap.block;
    data.blockEntity = cap.blockEntity;
    carry::PickupTarget target{player, region, &region.getBlock(pos), &pos, &data.blockEntity};
    return carry::beforePickup(target, data);
}

/// The tick after an accepted click: the checks again (the world or the player may have changed in
/// between), then the script stage and the pickup itself. Protection already ran at the click.
void finish(Pending const& job) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto* player = level->getPlayer(job.player);
    if (!player || static_cast<int>(player->getDimensionId()) != job.dimension) return;
    auto&                region = player->getDimensionBlockSource();
    carry::CapturedBlock cap;
    if (checks(*player, region, job.pos, cap) != Refusal::None) return;
    if (cap.block != job.block) return; // another block since the click
    carry::CarryData data;
    if (!runScript(*player, region, job.pos, cap, data)) return;
    perform(*player, region, job.pos, std::move(data));
}

void runPending() {
    if (pending().empty()) return;
    auto jobs = std::move(pending());
    pending().clear();
    for (auto const& job : jobs) {
        try {
            finish(job);
        } catch (std::exception const& e) {
            log().error("blockpickup: pickup failed: {}", e.what());
        }
    }
}

void onInteract(ll::event::PlayerInteractBlockEvent& ev) {
    auto& player = ev.self();
    if (!serverSide(player)) return;
    // Carrying: the placement units (U2 / U4) own the click.
    if (carry::isCarrying(player)) return;
    BlockPos const pos = ev.blockPos();
    // A pickup accepted earlier in this tick swallows the repeats on the same block.
    if (isPendingAt(player, pos)) {
        ev.cancel();
        return;
    }
    auto&                region = player.getDimensionBlockSource();
    carry::CapturedBlock cap;
    if (checks(player, region, pos, cap) != Refusal::None) return; // silent; the vanilla use proceeds
    if (!protectionAllows(player, region, pos, cap)) return;
    // Accepted: the block is not used. The world edit (and the script stage, whose commandPickup
    // may edit the world too) runs next tick, as a cancelled interaction rolls back block changes.
    ev.cancel();
    pending().push_back(Pending{player.getUuid(), static_cast<int>(player.getDimensionId()), pos, std::move(cap.block)});
}

} // namespace

std::string_view refusalName(Refusal r) {
    switch (r) {
    case Refusal::None: return "none";
    case Refusal::Preconditions: return "preconditions";
    case Refusal::NotLoaded: return "not loaded";
    case Refusal::Lists: return "lists";
    case Refusal::DoubleHeight: return "double height";
    case Refusal::Unbreakable: return "unbreakable";
    case Refusal::NoBlockEntity: return "no block entity";
    case Refusal::Locked: return "locked";
    case Refusal::CustomCondition: return "custom condition";
    case Refusal::Protection: return "protection";
    case Refusal::Script: return "script";
    }
    return "?";
}

bool hasLock(std::string const& blockEntitySnbt) {
    auto tag = carry::parseNbt(blockEntitySnbt);
    return tag && tag->contains("Lock");
}

bool isPending(Player const& player) {
    auto const& uuid = player.getUuid();
    for (auto const& p : pending())
        if (p.player == uuid) return true;
    return false;
}

Refusal evaluate(Player& player, BlockSource& region, BlockPos const& pos, carry::CarryData& data) {
    carry::CapturedBlock cap;
    if (auto r = checks(player, region, pos, cap); r != Refusal::None) return r;
    if (!protectionAllows(player, region, pos, cap)) return Refusal::Protection;
    if (!runScript(player, region, pos, cap, data)) return Refusal::Script;
    return Refusal::None;
}

bool perform(Player& player, BlockSource& region, BlockPos const& pos, carry::CarryData data) {
    if (carry::isCarrying(player)) return false;
    // 13. the state and block actor as they are now (water stays in the liquid layer).
    auto cap = carry::captureBlock(region, pos);
    if (!cap) return false;
    Block const& block = region.getBlock(pos); // registry block: outlives the removal
    if (block.isAir()) return false;
    std::string id   = game::blockId(block);
    data.type        = carry::CarryType::Block;
    data.block       = std::move(cap->block);
    data.blockEntity = std::move(cap->blockEntity);
    // Stored before the block goes: a failure here never loses the block and its contents.
    carry::set(player, std::move(data));

    // 14. remove the block actor and the block, nothing drops.
    if (!carry::removeCarriedBlock(region, pos)) {
        carry::clear(player);
        log().warn("blockpickup: could not remove {} at {} {} {}", id, pos.x, pos.y, pos.z);
        return false;
    }

    // 15. hit sound at the block, swing.
    carry::playBlockSound(player, pos, block, carry::BlockSound::Hit);
    carry::swing(player);
    // 16. Slowness from the stored record (applySlowness honours slownessInCreative).
    carry::applySlowness(player, carry::blockAmplifier(carry::get(player)));
    // 17.
    log().info("carry: {} picked up block {} at {} {} {}", player.getRealName(), id, pos.x, pos.y, pos.z);
    return true;
}

/// Listens to use-block and finishes accepted pickups on the next server tick.
class BlockPickupModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([](auto& ev) {
            try {
                onInteract(ev);
            } catch (std::exception const& e) {
                log().error("blockpickup: use-block handler failed: {}", e.what());
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runPending(); }));
        // The client loads many worlds in one process: never carry a click into the next world.
        static bool closeHooked = false;
        if (!closeHooked) {
            closeHooked = true;
            game::db::onWorldClosed([] { pending().clear(); });
        }
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        pending().clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::feature::blockpickup

LO_REGISTER_MODULE(lo::feature::blockpickup::BlockPickupModule);
