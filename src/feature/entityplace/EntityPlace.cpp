// U4 entityplace: placing a carried entity (docs/SPEC.md §3.7) and entity stacking (§3.8).

#include "feature/entityplace/EntityPlace.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/entity/components_json_legacy/MountTamingComponent.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Feedback.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::feature::entityplace {

namespace {
using carry::CarryData;

/// Java: block.lava.pop (0.5, 0.5) at the player.
void lavaPop(Player& player) { carry::playSound(player, carry::sound::kLavaPop, 0.5f, 0.5f); }

BlockPos neighbour(BlockPos const& pos, int face) {
    switch (face) {
    case 0: return {pos.x, pos.y - 1, pos.z};
    case 1: return {pos.x, pos.y + 1, pos.z};
    case 2: return {pos.x, pos.y, pos.z - 1};
    case 3: return {pos.x, pos.y, pos.z + 1};
    case 4: return {pos.x - 1, pos.y, pos.z};
    case 5: return {pos.x + 1, pos.y, pos.z};
    default: return pos;
    }
}

int blockDistSq(BlockPos const& a, BlockPos const& b) {
    int dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float area(Actor const& a) { return carry::entityHeight(a) * carry::entityWidth(a); }

/// The carried entity created from its NBT at `pos` but not yet added to the world (Java's
/// `CarryOnData.getEntity`): its size can be checked before it appears.
OwnerPtr<EntityContext> loadDetached(BlockSource& region, Vec3 const& pos, std::string const& snbt) {
    auto tag = carry::parseNbt(snbt);
    if (!tag) return {};
    (*tag)["Pos"] = ListTag{pos.x, pos.y, pos.z};
    auto level    = ll::service::getLevel();
    if (!level) return {};
    DefaultDataLoadHelper helper; // keeps the UniqueID
    return level->getActorFactory().loadActor(&*tag, helper, region.getDimension().mHeightRange, nullptr);
}

/// Step 6: a horse at the top becomes tamed (Java `setTamed(true)`: no owner).
void tameHorse(Actor& top) {
    if (top.getTypeName() != "minecraft:horse" || top.isTame()) return;
    if (auto taming = top.getEntityContext().tryGetComponent<MountTamingComponent>()) taming->becomeTame(top, false);
}
} // namespace

std::string_view stackResultName(StackResult r) {
    switch (r) {
    case StackResult::NotCarrying: return "not_carrying";
    case StackResult::Disabled: return "disabled";
    case StackResult::Full: return "full";
    case StackResult::OnSelf: return "on_self";
    case StackResult::Forbidden: return "forbidden";
    case StackResult::TooBig: return "too_big";
    case StackResult::Failed: return "failed";
    case StackResult::Stacked: return "stacked";
    }
    return "?";
}

StackResult tryStack(Player& carrier, Actor& clicked) {
    CarryData data = carry::get(carrier);
    if (!data.isEntity() && !data.isPlayer()) return StackResult::NotCarrying;
    auto const& s = config::get<carry::CarryConfig>().settings;
    // 1. Held entity: the carried entity, or the carried player (the carrier's first passenger).
    if (!s.stackableEntities) return StackResult::Disabled;
    Actor* heldPlayer = nullptr;
    if (data.isPlayer()) {
        heldPlayer = carrier.getFirstPassenger();
        if (!heldPlayer) return StackResult::NotCarrying;
    }

    // 2. Count the passengers up the first-passenger chain of the column.
    Actor& root  = rootVehicleOf(clicked);
    Actor* top   = &root;
    int    count = 0;
    for (Actor* p = firstPassengerOf(root); p && count < 4096; p = firstPassengerOf(*p)) {
        ++count;
        top = p;
    }
    if (count >= s.maxEntityStackLimit - 1) {
        lavaPop(carrier);
        return StackResult::Full;
    }
    // 3. Top entity = the held entity: nothing.
    if (top == heldPlayer) return StackResult::OnSelf;
    // 4. Stacking lists (silent).
    if (!carry::isStackingAllowed(*top)) return StackResult::Forbidden;

    // 7. Where the held entity appears: 2.6 above the clicked entity when the player is close.
    Vec3 const clickedPos = clicked.getFeetPos();
    bool const near       = blockDistSq(clicked.getFeetBlockPos(), carrier.getFeetBlockPos()) < 6;
    Vec3 const spawnAt    = near ? Vec3{clickedPos.x, clickedPos.y + 2.6f, clickedPos.z} : clickedPos;
    auto&      region     = clicked.getDimensionBlockSource();

    // (constructed in place: EntityContext has no usable assignment)
    std::optional<OwnerPtr<EntityContext>> detached;
    Actor*                                 held = heldPlayer;
    if (!heldPlayer) {
        detached.emplace(loadDetached(region, spawnAt, data.entity));
        held = detached->tryUnwrap<Actor, true>().as_ptr();
        if (!held) {
            log().warn("carry: could not recreate the carried entity of {} for stacking", carrier.getRealName());
            return StackResult::Failed;
        }
    }
    // 5. Size: the held entity must not be bigger than the top one.
    if (s.entitySizeMattersStacking && area(*held) > area(*top)) {
        lavaPop(carrier);
        return StackResult::TooBig; // the detached entity is discarded
    }
    // 6. Horse at the top: tamed.
    tameHorse(*top);

    // 7. Spawn (or move the carried player) at the clicked entity.
    if (heldPlayer) {
        heldPlayer->stopRiding(true, false, false, false);
        net::broadcastStartRidingOther(carrier, *heldPlayer, false);
        heldPlayer->teleport(spawnAt, clicked.getDimensionId());
        if (near) heldPlayer->teleport(clickedPos, clicked.getDimensionId());
    } else {
        auto level = ll::service::getLevel();
        held       = level ? level->addEntity(region, std::move(*detached)) : nullptr;
        if (!held) {
            log().warn("carry: could not add the carried entity of {} for stacking", carrier.getRealName());
            return StackResult::Failed;
        }
        if (near) held->teleport(clickedPos, clicked.getDimensionId());
    }

    // 8. Ride the top entity.
    if (ride(*held, *top)) {
        if (held->isRiding(top)) net::broadcastStartRidingOther(*top, *held, true);
    } else {
        log().info("carry: {} could not ride {}; it stays placed", held->getTypeName(), top->getTypeName());
    }

    // 9. commandPlace, swing, clear, sound, Slowness.
    carry::runOnPlace(carrier, data);
    carry::swing(carrier);
    carry::clear(carrier);
    carry::playSound(carrier, carry::sound::kHorseSaddle, 0.5f, 1.5f);
    carry::removeSlowness(carrier);
    log().info("carry: {} stacked {} on {}", carrier.getRealName(), held->getTypeName(), top->getTypeName());
    return StackResult::Stacked;
}

bool tryPlaceEntity(Player& carrier, BlockPos const& clicked, int face) {
    CarryData data = carry::get(carrier);
    if (!data.isEntity()) return false;
    auto& region = carrier.getDimensionBlockSource();

    // 1. The clicked block, or its neighbour on the clicked face.
    BlockPos target = clicked;
    if (!game::replaceable(region, target)) {
        target = neighbour(clicked, face);
        if (!game::replaceable(region, target)) {
            lavaPop(carrier);
            return false;
        }
    }
    // 2. Bottom centre, no collision check.
    Vec3 const pos = target.bottomCenter();
    // 4. Protection (hooks only; no built-in event for entities), commandPlace, spawn.
    if (!carry::placeAllowed(carry::PlaceTarget{carrier, region, target, data, face}, false)) return false;
    carry::runOnPlace(carrier, data);
    Actor* actor = carry::spawnActor(region, pos, data.entity);
    if (!actor) {
        log().warn("carry: could not recreate the carried entity of {}; it stays carried", carrier.getRealName());
        return false;
    }
    carry::playAmbient(*actor);
    carry::swing(carrier);
    carry::clear(carrier);
    carry::removeSlowness(carrier);
    log().info("carry: {} placed entity {} at {} {} {}", carrier.getRealName(), actor->getTypeName(), target.x, target.y, target.z);
    return true;
}

namespace {

/// A use-block placement waiting for the next tick (a cancelled interaction rolls back world edits).
struct PendingPlace {
    std::string uuid;
    BlockPos    pos;
    int         face;
    int         dimension;
};

std::vector<PendingPlace>& pending() {
    static std::vector<PendingPlace> v;
    return v;
}

void runPending() {
    if (pending().empty()) return;
    auto list  = std::exchange(pending(), {});
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto const& p : list) {
        Player* player = level->getPlayer(mce::UUID::fromString(p.uuid));
        if (!player || static_cast<int>(player->getDimensionId()) != p.dimension) continue;
        if (!carry::get(*player).isEntity() || !carry::canAct(*player)) continue;
        tryPlaceEntity(*player, p.pos, p.face);
    }
}

} // namespace

// Entity interact while carrying an entity: stacking (§3.8). The vanilla interaction is cancelled
// whenever an entity is carried (hands are full). Carried players are left to U5 (it may call
// `tryStack`); carried blocks / no carry fall through to the other units and vanilla.
LL_TYPE_INSTANCE_HOOK(
    EntityStackInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&        entity,
    Vec3 const&   location,
    ::HandSlot    handSlot
) {
    Player& player = mPlayer;
    if (!player.isClientSide() && carry::get(player).isEntity()) {
        if (carry::canAct(player)) {
            auto r = tryStack(player, entity);
            if (r != StackResult::Stacked && r != StackResult::NotCarrying)
                log().debug("carry: stacking by {} refused ({})", player.getRealName(), stackResultName(r));
        }
        return false;
    }
    return origin(entity, location, handSlot);
}

/// Listens to use-block and entity interact while carrying an entity.
class EntityPlaceModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mInteract = bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([](ll::event::PlayerInteractBlockEvent& ev) {
            Player& player = ev.self();
            if (player.isClientSide()) return;
            auto const& data = carry::get(player);
            // Use-block is denied for every entity / player carry (U2 does the same for blocks; the
            // clicked block is never used). Placing a carried player is U5's.
            if (!data.isEntity() && !data.isPlayer()) return;
            ev.cancel();
            if (!data.isEntity() || !carry::canAct(player)) return;
            pending().push_back({
                player.getUuid().asString(),
                ev.blockPos(),
                static_cast<int>(ev.face()),
                static_cast<int>(player.getDimensionId()),
            });
        });
        mTick = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runPending(); });
        EntityStackInteractHook::hook();
    }
    void uninstall() {
        EntityStackInteractHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        if (mInteract) bus.removeListener(mInteract);
        if (mTick) bus.removeListener(mTick);
        mInteract = nullptr;
        mTick     = nullptr;
        pending().clear();
    }

private:
    ll::event::ListenerPtr mInteract;
    ll::event::ListenerPtr mTick;
};

} // namespace lo::feature::entityplace

LO_REGISTER_MODULE(lo::feature::entityplace::EntityPlaceModule);
