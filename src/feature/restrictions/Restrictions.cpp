// U6: restrictions while carrying (docs/SPEC.md §3.4). See Restrictions.h for the rules.

#include "feature/restrictions/Restrictions.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/entity/ActorHurtEvent.h"
#include "ll/api/event/player/PlayerAttackEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerPickUpItemEvent.h"
#include "ll/api/event/player/PlayerUseItemEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/shared_types/legacy/ContainerType.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetEventCallback.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/ActorPickRequestPacket.h"
#include "mc/network/packet/BlockPickRequestPacket.h"
#include "mc/network/packet/ContainerClosePacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerHotbarPacket.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/ContainerID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/managers/models/ContainerManagerModel.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/PlaceCarried.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::restrictions {

namespace {

/// True for a player of the server (BDS or the integrated server); false for the client's own copy of a
/// player in the client build (the carry store lives on the server thread only).
bool serverSide(Player const& player) { return player.mLevel && !player.getLevel().isClientSide(); }

/// The server player carries something.
bool carrying(Player const& player) { return serverSide(player) && carry::isCarrying(player); }

bool hitAllowed() { return config::get<carry::CarryConfig>().settings.hitWhileCarrying; }

constexpr int kHotbarSize = 9;

bool validSlot(int slot) { return slot >= 0 && slot < kHotbarSize; }

/// Logs one line per (player, kind) at most every 10 ticks: a held key or a client that keeps fighting
/// the lock must not log every tick.
template <class... Args>
void logAction(Player const& player, std::string_view kind, fmt::format_string<Args...> fmt, Args&&... args) {
    static std::unordered_map<std::string, std::int64_t> last;
    auto  key  = std::to_string(player.getOrCreateUniqueID().rawID) + ':' + std::string(kind);
    auto  now  = carry::tickCount(player);
    auto  it   = last.find(key);
    if (it != last.end() && now >= it->second && now - it->second < 10) return;
    last[key] = now;
    if (last.size() > 4096) last.clear();
    log().info(fmt, std::forward<Args>(args)...);
}

/// A full stack used to mark the locked slot as occupied (built once per world: item registries
/// differ between the client's local worlds).
ItemStack const& placeholderItem() {
    static std::optional<ItemStack> item;
    static void const*              itemOf = nullptr;
    auto                            level  = ll::service::getLevel();
    void const*                     now    = level ? level.as_ptr() : nullptr;
    if (!item || itemOf != now) {
        item   = game::makeItem("minecraft:barrier", 64);
        itemOf = now;
    }
    return *item;
}

/// While alive, the carrier's locked hotbar slot holds a full stack, so the vanilla insertion code
/// (`Inventory::add` / `canAdd` / `getFirstEmptySlot`) sees it as occupied and puts items elsewhere
/// (or nowhere). The placeholder goes in and out of the slot vector directly: no container change is
/// recorded or sent for that slot, and the slot is empty again before the hooked call returns.
class LockedSlotReservation {
public:
    explicit LockedSlotReservation(FillingContainer const& container) {
        Player* owner = container.mPlayer;
        if (!owner) return;
        auto& items = const_cast<std::vector<ItemStack>&>(container.mItems.get());
        if (!carrying(*owner)) return;
        int slot = carry::get(*owner).selected;
        if (!validSlot(slot) || slot >= static_cast<int>(items.size()) || !items[slot].isNull()) return;
        auto const& placeholder = placeholderItem();
        if (placeholder.isNull()) return;
        items[slot] = placeholder;
        mItems      = &items;
        mSlot       = slot;
    }
    ~LockedSlotReservation() {
        if (mItems) (*mItems)[mSlot] = ItemStack::EMPTY_ITEM();
    }
    LockedSlotReservation(LockedSlotReservation const&)            = delete;
    LockedSlotReservation& operator=(LockedSlotReservation const&) = delete;

private:
    std::vector<ItemStack>* mItems = nullptr;
    int                     mSlot  = -1;
};

/// A slot change request of `player` (MobEquipmentPacket): true = refused (and the client told to
/// select the locked slot again).
bool refuseSlotChange(Player& player, ContainerID container, int slot) {
    if (container != ContainerID::Inventory || !carrying(player)) return false;
    int locked = carry::get(player).selected;
    if (!validSlot(locked) || slot == locked) return false;
    selectSlot(player, locked);
    logAction(player, "slot", "restrictions: kept {} on hotbar slot {} (asked for {})", player.getRealName(), locked, slot);
    return true;
}

ServerPlayer* senderOf(ServerNetworkHandler* handler, NetworkIdentifier const& source, Packet const& packet) {
    return handler->_getServerPlayer(source, packet.mSenderSubId);
}

/// Pick block / pick entity: refused while carrying (the hand stays empty).
bool refusePick(ServerPlayer* player) {
    if (!player || !carrying(*player)) return false;
    logAction(*player, "pick", "restrictions: refused pick block of {} (carrying)", player->getRealName());
    return true;
}

/// Tells the client to close a container screen (server-initiated close).
void sendClose(Player& player, ContainerID id, SharedTypes::Legacy::ContainerType type) {
    auto pkt = MinecraftPackets::createPacket(MinecraftPacketIds::ContainerClose);
    if (!pkt) return;
    auto& close                 = static_cast<ContainerClosePacket&>(*pkt);
    close.mContainerId          = id;
    close.mContainerType        = type;
    close.mServerInitiatedClose = true;
    close.sendTo(player);
}

// ---- hooks ------------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(InventoryAddHook, ll::memory::HookPriority::Normal, Inventory, &Inventory::$add, bool, ItemStack& item) {
    LockedSlotReservation reserve(*this);
    return origin(item);
}

LL_TYPE_INSTANCE_HOOK(
    InventoryCanAddHook,
    ll::memory::HookPriority::Normal,
    Inventory,
    &Inventory::$canAdd,
    bool,
    ItemStack const& item
) {
    LockedSlotReservation reserve(*this);
    return origin(item);
}

LL_TYPE_INSTANCE_HOOK(
    InventoryFirstEmptySlotHook,
    ll::memory::HookPriority::Normal,
    Inventory,
    &Inventory::$getFirstEmptySlot,
    int
) {
    LockedSlotReservation reserve(*this);
    return origin();
}

LL_TYPE_INSTANCE_HOOK(
    MobEquipmentHook,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&             source,
    std::shared_ptr<MobEquipmentPacket> packet
) {
    if (packet) {
        if (auto* player = senderOf(thisFor<NetEventCallback>(), source, *packet)) {
            if (refuseSlotChange(*player, packet->mContainerId, packet->mSelectedSlot)) return;
        }
    }
    origin(source, std::move(packet));
}

LL_TYPE_INSTANCE_HOOK(
    BlockPickHook,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&      source,
    BlockPickRequestPacket const& packet
) {
    if (refusePick(senderOf(thisFor<NetEventCallback>(), source, packet))) return;
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    ActorPickHook,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&      source,
    ActorPickRequestPacket const& packet
) {
    if (refusePick(senderOf(thisFor<NetEventCallback>(), source, packet))) return;
    origin(source, packet);
}

/// The own inventory screen (InteractPacket OpenInventory) is not opened while carrying; a close is
/// sent in case the client already shows it.
LL_TYPE_INSTANCE_HOOK(OpenInventoryHook, ll::memory::HookPriority::Normal, ServerPlayer, &ServerPlayer::$openInventory, void) {
    if (carrying(*this)) {
        sendClose(*this, ContainerID::Inventory, SharedTypes::Legacy::ContainerType::Inventory);
        logAction(*this, "inventory", "restrictions: refused the inventory of {} (carrying)", getRealName());
        return;
    }
    origin();
}

/// Break speed 0 while carrying (unless hitWhileCarrying).
LL_TYPE_INSTANCE_HOOK(
    DestroyProgressHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::getDestroyProgress,
    float,
    Block const& block
) {
    if (!hitAllowed() && carrying(*this)) return 0.0f;
    return origin(block);
}

/// Item use (the LeviLamina event does not cover every path, e.g. `SimulatedPlayer::simulateUseItem`).
LL_TYPE_INSTANCE_HOOK(
    UseItemHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItem,
    bool,
    ItemStack& item,
    HandSlot   hand
) {
    Player& player = mPlayer;
    if (carrying(player)) {
        logAction(player, "use", "restrictions: refused item use of {} (carrying)", player.getRealName());
        return false;
    }
    return origin(item, hand);
}

using Hooks = ll::memory::HookRegistrar<
    UseItemHook,
    InventoryAddHook,
    InventoryCanAddHook,
    InventoryFirstEmptySlotHook,
    MobEquipmentHook,
    BlockPickHook,
    ActorPickHook,
    OpenInventoryHook,
    DestroyProgressHook>;

} // namespace

void selectSlot(Player& player, int slot) {
    player.setSelectedSlot(slot);
    auto pkt = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerHotbar);
    if (!pkt) return;
    auto& hotbar             = static_cast<PlayerHotbarPacket&>(*pkt);
    hotbar.mSelectedSlot     = static_cast<uint>(slot);
    hotbar.mShouldSelectSlot = true;
    hotbar.mContainerId      = ContainerID::Inventory;
    hotbar.sendTo(player);
}

bool closeContainer(Player& player) {
    std::shared_ptr<ContainerManagerModel> manager = player.mContainerManager;
    if (!manager || !serverSide(player)) return false;
    sendClose(player, manager->getContainerId(), manager->getContainerType());
    static_cast<ServerPlayer&>(player).doDeleteContainerManager(false);
    return true;
}

class RestrictionsModule {
public:
    void install() {
        mHooks.emplace(); // hooks on construction
        auto& bus = ll::event::EventBus::getInstance();

        mListeners.push_back(bus.emplaceListener<ll::event::PlayerUseItemEvent>([](ll::event::PlayerUseItemEvent& ev) {
            if (!carrying(ev.self())) return;
            ev.cancel();
            logAction(ev.self(), "use", "restrictions: refused item use of {} (carrying)", ev.self().getRealName());
        }));

        mListeners.push_back(bus.emplaceListener<ll::event::PlayerAttackEvent>([](ll::event::PlayerAttackEvent& ev) {
            if (hitAllowed() || !carrying(ev.self())) return;
            ev.cancel();
            logAction(ev.self(), "attack", "restrictions: refused an attack of {} (carrying)", ev.self().getRealName());
        }));

        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>(
            [](ll::event::PlayerDestroyBlockEvent& ev) {
                if (carry::inProtectionCheck() || hitAllowed() || !carrying(ev.self())) return;
                ev.cancel();
                auto const& pos = ev.pos();
                logAction(
                    ev.self(),
                    "destroy",
                    "restrictions: refused breaking {} {} {} by {} (carrying)",
                    pos.x,
                    pos.y,
                    pos.z,
                    ev.self().getRealName()
                );
            }
        ));

        // A returning item (e.g. a Loyalty trident) prefers the slot it was thrown from: never the
        // locked slot. Every other way into the inventory goes through the reservation above.
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPickUpItemEvent>([](ll::event::PlayerPickUpItemEvent& ev) {
            if (!carrying(ev.self())) return;
            if (ev.favoredSlot() == carry::get(ev.self()).selected) ev.favoredSlot() = -1;
        }));

        // dropCarriedWhenHit: remember the carrier; placed at the next tick if still alive ([LO]: the hurt
        // event fires before the damage is applied; one tick later a carrier killed by the hit is dead and
        // left to U7's death placement, SPEC §9 "U6 skips a carrier that is dying").
        mListeners.push_back(bus.emplaceListener<ll::event::ActorHurtEvent>(
            [this](ll::event::ActorHurtEvent& ev) {
                if (ev.isCancelled() || ev.damage() <= 0.0f) return;
                if (!config::get<carry::CarryConfig>().settings.dropCarriedWhenHit) return;
                auto& actor = ev.self();
                if (!actor.isPlayer()) return;
                auto& player = static_cast<Player&>(actor);
                // Creative / invulnerable carriers take no damage, so nothing is dropped.
                if (!carrying(player) || player.isInvulnerableTo(ev.source())) return;
                mHit.insert(player.getOrCreateUniqueID().rawID);
            },
            ll::event::EventPriority::Lowest
        ));

        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](ll::event::ServerLevelTickEvent&) {
            tick();
        }));
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        mHooks.reset();
        mHit.clear();
    }

private:
    void tick() {
        if (!mHit.empty()) {
            auto hit = std::exchange(mHit, {});
            if (auto level = ll::service::getLevel()) {
                for (auto id : hit) {
                    auto* player = level->getPlayer(ActorUniqueID(id));
                    // A carrier that died is U7's (death placement at respawn).
                    if (!player || !player->isAlive() || !carrying(*player)) continue;
                    auto r = carry::placeCarried(*player);
                    if (r.placed) {
                        log().info(
                            "carry: placed {} of {} at {} {} {} (hit)",
                            r.what,
                            player->getRealName(),
                            r.x,
                            r.y,
                            r.z
                        );
                    }
                }
            }
        }

        carry::forEachCarrier([](Player& player, carry::CarryData const& data) {
            if (!serverSide(player)) return;
            int locked = data.selected;
            if (validSlot(locked) && player.getSelectedItemSlot() != locked) {
                int was = player.getSelectedItemSlot();
                selectSlot(player, locked);
                logAction(player, "slot", "restrictions: kept {} on hotbar slot {} (was {})", player.getRealName(), locked, was);
            }
            if (closeContainer(player)) {
                logAction(player, "container", "restrictions: closed the container screen of {} (carrying)", player.getRealName());
            }
        });
    }

    std::optional<Hooks>                mHooks;
    std::vector<ll::event::ListenerPtr> mListeners;
    std::unordered_set<std::int64_t>    mHit;
};

} // namespace lo::feature::restrictions

LO_REGISTER_MODULE(lo::feature::restrictions::RestrictionsModule);
