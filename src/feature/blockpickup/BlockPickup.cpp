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

/// A pickup accepted in a use-block event, performed on the next server tick.
struct Pending {
    mce::UUID         player;
    int               dimension = 0;
    BlockPos          pos;
    Block const*      block = nullptr; ///< registry block seen at the click (must still be there)
    carry::CarryData  data;
};

std::vector<Pending>& pending() {
    static std::vector<Pending> list;
    return list;
}

bool serverSide(Player const& player) { return !player.isClientSide() && !player.getLevel().isClientSide(); }

void runPending() {
    if (pending().empty()) return;
    auto jobs = std::move(pending());
    pending().clear();
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto& job : jobs) {
        auto* player = level->getPlayer(job.player);
        if (!player || static_cast<int>(player->getDimensionId()) != job.dimension) continue;
        auto& region = player->getDimensionBlockSource();
        if (&region.getBlock(job.pos) != job.block) continue; // changed since the click
        try {
            perform(*player, region, job.pos, std::move(job.data));
        } catch (std::exception const& e) {
            log().error("blockpickup: pickup failed: {}", e.what());
        }
    }
}

void onInteract(ll::event::PlayerInteractBlockEvent& ev) {
    auto& player = ev.self();
    if (!serverSide(player)) return;
    // Carrying: placement units (U2 / U4) own the click. A pickup accepted earlier in this tick
    // swallows the repeats until it is performed.
    if (carry::isCarrying(player)) return;
    if (isPending(player)) {
        ev.cancel();
        return;
    }
    auto&            region = player.getDimensionBlockSource();
    BlockPos const   pos    = ev.blockPos();
    carry::CarryData data;
    if (evaluate(player, region, pos, data) != Refusal::None) return; // silent; vanilla use proceeds
    ev.cancel();
    pending().push_back(Pending{
        player.getUuid(),
        static_cast<int>(player.getDimensionId()),
        pos,
        &region.getBlock(pos),
        std::move(data),
    });
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
    auto const& s = config::get<carry::CarryConfig>().settings;
    // 1-2. distance (to the block centre), game mode, empty hands, not carrying, tick guard, key.
    if (!carry::canStartPickup(player, pos.center(), false)) return Refusal::Preconditions;

    // 3. block actor NBT.
    auto cap = carry::captureBlock(region, pos);
    if (!cap) return Refusal::NotLoaded;
    Block const& block = region.getBlock(pos);
    if (block.isAir()) return Refusal::NotLoaded;

    carry::PickupTarget target{player, region, &block, &pos, &cap->blockEntity};
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
    bool hasBlockEntity = cap->hasBlockEntity() && !carry::isBedrockOnlyBlockActor(game::blockId(block));
    if (!override && !hasBlockEntity && !s.pickupAllBlocks) return Refusal::NoBlockEntity;
    // 9. locked containers.
    if (cap->hasBlockEntity() && hasLock(cap->blockEntity)) return Refusal::Locked;
    // 10. custom pickup conditions.
    if (!carry::customConditions(target)) return Refusal::CustomCondition;
    // 11. protection (publishes PlayerDestroyBlockEvent).
    if (!carry::protectionAllows(target)) return Refusal::Protection;

    data             = carry::get(player); // keeps keyPressed / selected
    data.clear();
    data.type        = carry::CarryType::Block;
    data.block       = std::move(cap->block);
    data.blockEntity = std::move(cap->blockEntity);
    // 12. script conditions, activation, commandPickup.
    if (!carry::beforePickup(target, data)) return Refusal::Script;
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

    // 14. remove the block actor and the block, nothing drops.
    if (!carry::removeCarriedBlock(region, pos)) {
        log().warn("blockpickup: could not remove {} at {} {} {}", id, pos.x, pos.y, pos.z);
        return false;
    }
    carry::set(player, std::move(data));

    // 15. hit sound at the block, swing.
    carry::playBlockSound(player, pos, block, carry::BlockSound::Hit);
    carry::swing(player);
    // 16. Slowness from the stored record (applySlowness honours slownessInCreative).
    carry::applySlowness(player, carry::blockAmplifier(carry::get(player)));
    // 17.
    log().info("carry: {} picked up block {} at {} {} {}", player.getRealName(), id, pos.x, pos.y, pos.z);
    return true;
}

/// Listens to use-block and performs accepted pickups on the next server tick.
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
