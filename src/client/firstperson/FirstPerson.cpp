#ifdef LO_CLIENT

#include "client/firstperson/FirstPerson.h"

#include <chrono>
#include <cmath>
#include <unordered_map>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "glm/gtc/constants.hpp"
#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/world/level/ActorGarbageCollector.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveActorPacketPayload.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/WeakEntityRef.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/provider/ActorMovement.h"
#include "mc/world/actor/provider/PlayerMovement.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BaseMobSpawner.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/DataItem2.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ShortTag.h"
#include "mc/world/level/dimension/Dimension.h"

#include "client/Client.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Slowness.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "net/CarryProtocol.h"

namespace lo::client::firstperson {

namespace carry = lo::feature::carry;
using carry::CarryData;
using carry::CarryVisual;

// ---- transforms (SPEC §4.2) ---------------------------------------------------------------------

namespace {
constexpr glm::vec3 kX{1.f, 0.f, 0.f};
constexpr glm::vec3 kY{0.f, 1.f, 0.f};
constexpr glm::vec3 kZ{0.f, 0.f, 1.f};

void rotate(glm::mat4& m, float degrees, glm::vec3 const& axis) { m = glm::rotate(m, glm::radians(degrees), axis); }
} // namespace

bool isChest(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return id == "chest" || id == "trapped_chest" || id == "ender_chest" || id.ends_with("copper_chest");
}

void applyScriptTransform(glm::mat4& m, CarryVisual const& v) {
    rotate(m, v.rotation[0], kX);
    rotate(m, v.rotation[1], kY);
    rotate(m, v.rotation[2], kZ);
    m = glm::translate(m, glm::vec3{v.translation[0], v.translation[1], v.translation[2]});
    m = glm::scale(m, glm::vec3{v.scale[0], v.scale[1], v.scale[2]});
}

glm::mat4 blockTransform(bool facePlayer, bool chest, CarryVisual const& visual) {
    glm::mat4 m{1.f};
    m = glm::scale(m, glm::vec3{2.5f});
    m = glm::translate(m, glm::vec3{0.f, -0.5f, -1.f});
    if (facePlayer != chest) {
        rotate(m, 180.f, kY);
        rotate(m, -8.f, kX);
    } else {
        rotate(m, 8.f, kX);
    }
    applyScriptTransform(m, visual);
    return m;
}

glm::mat4 viewBobTransform(float walkDist, float bob) {
    // Java GameRenderer#bobView, applied to the hand when the "view bobbing" option is on.
    constexpr float pi = glm::pi<float>();
    float           f1 = -walkDist;
    glm::mat4       m{1.f};
    m = glm::translate(m, glm::vec3{std::sin(f1 * pi) * bob * 0.5f, -std::abs(std::cos(f1 * pi) * bob), 0.f});
    rotate(m, std::sin(f1 * pi) * bob * 3.f, kZ);
    rotate(m, std::abs(std::cos(f1 * pi - 0.2f) * bob) * 5.f, kX);
    return m;
}

glm::mat4 entityTransform(float h, float w, bool rotateSideways, CarryVisual const& visual) {
    glm::mat4 m{1.f};
    rotate(m, 180.f, kY);
    m = glm::scale(m, glm::vec3{0.8f});
    m = glm::translate(m, glm::vec3{0.f, -h - 0.2f, w * 1.3f + 0.1f});
    applyScriptTransform(m, visual);
    if (rotateSideways) rotate(m, 90.f, kY);
    return m;
}

// ---- renderer state (client thread) -------------------------------------------------------------

namespace {

/// A mob spawner that is never placed: the game's own way to build a fully initialised client-only
/// actor of a type (vanilla draws the mob spinning in a spawner with it, without adding it to the
/// level's entity list). One per level, never destroyed (its display entity is removed instead).
class CarrySpawner : public BaseMobSpawner {
public:
    explicit CarrySpawner(ActorDefinitionIdentifier const& id) : BaseMobSpawner(id) {}
    ~CarrySpawner() override = default;
    void             tick(BlockSource&) override {}
    void             load(CompoundTag const&) override {}
    void             save(CompoundTag&) override {}
    BlockPos const&  getPos() override { return mAt; }
    BlockPos         mAt;
};

/// The cached render state of the local player's carry.
struct Cache {
    std::optional<CarryData>     data;   ///< the carry the cache was built for
    CarryVisual                  visual; ///< its visual
    std::string                  what;   ///< item / entity id drawn (selftests, log)
    bool                         chest = false;
    ItemStack                    item;                ///< block / item visual
    std::optional<WeakEntityRef> entity;              ///< entity visual: a client-only display actor
    Level*                       level     = nullptr; ///< the level that owns the display actor
    CarrySpawner*                spawner   = nullptr; ///< builds the display actor (of `spawnerLevel`)
    Level*                       spawnerLevel = nullptr;
    int                          dimension = -1;      ///< the dimension the display actor lives in
    bool                         logged    = false;
    glm::mat4                    entityView{1.f};    ///< view-space transform of the carried entity
    bool                         wantEntity = false; ///< the first-person pass asked for the entity
    OwnerStorageEntity           owned;              ///< the real client entity, taken out of the level
};

/// Entities the server just removed near the local player, kept for a moment out of the level: when
/// the removal is a pickup by the local player, the carried entity is this very client actor (fully
/// initialised, with its variant, colours, age and equipment as the server sent them).
struct Stashed {
    OwnerStorageEntity                    owner;
    Level*                                level = nullptr;
    std::chrono::steady_clock::time_point at;
};
std::unordered_map<std::int64_t, Stashed>& stash() {
    static auto* s = new std::unordered_map<std::int64_t, Stashed>;
    return *s;
}
constexpr auto  kStashTime   = std::chrono::seconds(3);
constexpr float kStashRadius = 64.f;

/// Destroys an entity taken out of `level` (only while that level is the current one).
void destroyTaken(OwnerStorageEntity& owner, Level* level) {
    if (!owner.has_value()) return;
    auto* client = currentClient();
    if (level && client && client->getLevel() == level) {
        level->mActorGarbageCollector->garbageCollectEntity(OwnerPtr<EntityContext>(std::move(owner)));
    }
    owner.reset();
}

/// Drops stashed entities that nobody claimed in time (or all of them).
void pruneStash(bool all) {
    auto now = std::chrono::steady_clock::now();
    std::erase_if(stash(), [&](auto& kv) {
        if (!all && now - kv.second.at < kStashTime) return false;
        destroyTaken(kv.second.owner, kv.second.level);
        return true;
    });
}

Cache& cache() {
    // Never destroyed: the item stack must not be torn down after the game at process exit.
    static auto* c = new Cache;
    return *c;
}

std::mutex gStatusMutex;
DrawStatus gStatus;

void setStatus(DrawStatus::Kind kind, std::string const& what, bool count) {
    bool            alive = cache().entity.has_value();
    std::lock_guard lock(gStatusMutex);
    if (gStatus.kind != kind || gStatus.what != what) {
        gStatus.kind   = kind;
        gStatus.what   = what;
        gStatus.frames = 0;
    }
    if (count) ++gStatus.frames;
    gStatus.actorAlive = alive;
}

/// The fields of a carry that decide what is drawn (a re-sync that only refreshes the hotbar slot,
/// tick or key state keeps the cache and the display actor).
bool sameVisual(CarryData const& a, CarryData const& b) {
    return a.type == b.type && a.block == b.block && a.blockEntity == b.blockEntity && a.entity == b.entity
        && a.player == b.player && a.activeScript == b.activeScript && a.scriptRender == b.scriptRender;
}

Actor* cachedActor() {
    auto& c = cache();
    if (!c.entity.has_value()) return nullptr;
    return c.entity->tryUnwrap<Actor, true>().as_ptr();
}

/// Where the display actor waits between frames: above the build limit, out of sight (the level
/// renderer draws display actors too). It is moved to the player only while we draw it.
Vec3 parkPos(LocalPlayer& player) {
    auto const& p = player.getPosition();
    return Vec3{p.x, static_cast<float>(player.getDimension().mHeightRange->mMax + 256), p.z};
}

/// Removes the display actor. `levelAlive` false: the level (and the actor with it) is gone.
void releaseActor(bool levelAlive) {
    auto& c = cache();
    if (c.owned.has_value()) {
        if (levelAlive) destroyTaken(c.owned, c.level);
        c.owned.reset();
        c.entity.reset();
    }
    if (c.entity.has_value()) {
        if (levelAlive && c.spawner && c.spawnerLevel == c.level && cachedActor()) c.spawner->removeDisplayEntity();
        c.entity.reset();
    }
    c.level     = nullptr;
    c.dimension = -1;
}

void resetCache(bool levelAlive) {
    auto& c = cache();
    releaseActor(levelAlive);
    c.data.reset();
    c.visual = {};
    c.what.clear();
    c.item   = ItemStack{};
    c.chest      = false;
    c.logged     = false;
    c.wantEntity = false;
    setStatus(DrawStatus::Kind::None, {}, false);
}

/// Zeroes every rotation of the display actor (SPEC §4.1).
void zeroRotations(Actor& actor) {
    actor.setRotationWrapped(Vec2{0.f, 0.f});
    actor.setYHeadRotations(0.f, 0.f);
    if (actor.hasCategory(ActorCategory::Mob)) static_cast<Mob&>(actor).setYBodyRotations(0.f, 0.f);
}

/// Applies the parts of the carried NBT that change how an entity looks (variant, colours, baby,
/// saddle, shorn wool, chest, tamed) through the actor's synched data, as the server would send them.
void applyLook(Actor& actor, CompoundTag const& tag) {
    auto& data = actor.mEntityData;
    auto  intOf = [&](char const* key) -> std::optional<int> {
        if (!tag.contains(key)) return std::nullopt;
        auto const& t = tag.at(key);
        if (t.hold(Tag::Byte)) return static_cast<int>(t.get<ByteTag>().data);
        if (t.hold(Tag::Short)) return static_cast<int>(t.get<ShortTag>().data);
        if (t.hold(Tag::Int)) return t.get<IntTag>().data;
        return std::nullopt;
    };
    std::vector<std::unique_ptr<DataItem>> items;
    auto id = [](ActorDataIDs i) { return static_cast<DataItem::ID>(i); };
    if (auto v = intOf("Variant")) items.push_back(DataItem::create(id(ActorDataIDs::Variant), int{*v}));
    if (auto v = intOf("MarkVariant")) items.push_back(DataItem::create(id(ActorDataIDs::MarkVariant), int{*v}));
    if (auto v = intOf("Color")) items.push_back(DataItem::create(id(ActorDataIDs::ColorIndex), static_cast<schar>(*v)));
    if (auto v = intOf("Color2")) items.push_back(DataItem::create(id(ActorDataIDs::Color2Index), static_cast<schar>(*v)));
    auto flags = data->getInt64(static_cast<ushort>(ActorDataIDs::Reserved0));
    auto flag  = [&](char const* key, ActorFlags f) {
        auto v = intOf(key);
        if (!v) return;
        auto bit = std::int64_t{1} << static_cast<int>(f);
        flags    = *v ? (flags | bit) : (flags & ~bit);
    };
    flag("IsBaby", ActorFlags::Baby);
    flag("Saddled", ActorFlags::Saddled);
    flag("Sheared", ActorFlags::Sheared);
    flag("Chested", ActorFlags::Chested);
    flag("IsTamed", ActorFlags::Tamed);
    items.push_back(DataItem::create(id(ActorDataIDs::Reserved0), static_cast<int64>(flags)));
    // The same path as a SetActorDataPacket from the server.
    data->assignValues(items, actor, std::nullopt);
}

/// A client-only display actor of the carried entity (SPEC §4.1), built by a never-placed mob
/// spawner so it is initialised from its definition like the mob in a spawner. Its look comes from
/// the NBT (`applyLook`); rotations zeroed, hurt time 0, no name tag, no leash. `Actor::load` is not
/// used: it crashes on a client display actor.
void buildActor(LocalPlayer& player, std::string const& snbt) {
    auto& c     = cache();
    auto* level = currentClient() ? currentClient()->getLevel() : nullptr;
    if (!level) return;
    auto id = carry::entityIdOf(snbt);
    if (id.empty()) return;
    auto tag = carry::parseNbt(snbt);

    // The client's own copy of the entity the server just removed for the pickup.
    if (tag && tag->contains("UniqueID") && tag->at("UniqueID").hold(Tag::Int64)) {
        auto uid = tag->at("UniqueID").get<Int64Tag>().data;
        if (auto it = stash().find(uid); it != stash().end() && it->second.level == level) {
            c.owned.emplace(*it->second.owner);
            it->second.owner.reset();
            stash().erase(it);
            c.level     = level;
            c.dimension = static_cast<int>(player.getDimensionId());
            c.entity    = c.owned->getWeakRef();
            if (auto* actor = cachedActor()) {
                actor->setNameTag("");
                zeroRotations(*actor);
                log().info("firstperson: carrying the client's own {}", id);
                return;
            }
            releaseActor(true);
        }
    }

    ActorDefinitionIdentifier def(id);
    if (!c.spawner || c.spawnerLevel != level) {
        c.spawner      = new CarrySpawner(def); // the old level's spawner (if any) is abandoned with its level
        c.spawnerLevel = level;
    } else {
        c.spawner->removeDisplayEntity();
        c.spawner->setEntityId(def);
    }
    auto const& pos = player.getPosition();
    c.spawner->mAt  = BlockPos{static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                               static_cast<int>(std::floor(pos.z))};
    Actor* actor    = c.spawner->tryGetOrCreateDisplayEntity(player.getDimensionBlockSource());
    if (!actor) {
        log().warn("firstperson: cannot create a display actor for {}", id);
        return;
    }
    c.entity    = actor->getEntityContext().getWeakRef();
    c.level     = level;
    c.dimension = static_cast<int>(player.getDimensionId());
    if (tag) applyLook(*actor, *tag);
    actor->setNameTag("");
    zeroRotations(*actor);
    actor->_setPos(parkPos(player));
}

void rebuild(CarryData const& data, LocalPlayer& player) {
    resetCache(true);
    auto& c  = cache();
    c.data   = data;
    c.visual = carry::visualFor(data);
    switch (c.visual.kind) {
    case CarryVisual::Kind::Block: {
        // Carry On draws the item of the block's default state (nothing when it has no item).
        auto blockOnly = CarryData{.type = carry::CarryType::Block, .block = c.visual.block};
        c.chest        = isChest(carry::carriedBlockId(blockOnly));
        if (auto* state = carry::carriedBlock(blockOnly)) {
            auto def = Block::tryGetFromRegistry(HashedString{state->getTypeName()});
            c.item.reinit(def ? *def : *state, 1);
        }
        c.what = c.item.isNull() ? std::string{} : c.item.getTypeName();
        break;
    }
    case CarryVisual::Kind::Item:
        c.chest = isChest(carry::carriedBlockId(data));
        c.item  = game::makeItem(c.visual.item);
        c.what  = c.item.isNull() ? std::string{} : c.item.getTypeName();
        break;
    case CarryVisual::Kind::Entity:
        c.what = carry::entityIdOf(c.visual.entity);
        buildActor(player, c.visual.entity);
        break;
    default:
        break;
    }
}

/// Moves the display actor to the player for one draw and back to its park position afterwards
/// (also when the draw throws).
struct ActorAtPlayer {
    Actor&       actor;
    LocalPlayer& player;
    ActorAtPlayer(Actor& a, LocalPlayer& p) : actor(a), player(p) {
        actor.moveTo(player.getPosition(), Vec2{0.f, 0.f});
        zeroRotations(actor);
    }
    ~ActorAtPlayer() { actor.moveTo(parkPos(player), Vec2{0.f, 0.f}); }
    ActorAtPlayer(ActorAtPlayer const&)            = delete;
    ActorAtPlayer& operator=(ActorAtPlayer const&) = delete;
};

/// Draws the local player's carry instead of the hand. False: not carrying a block or an entity (the
/// vanilla hand renders; Carry On draws nothing in first person for a carried player).
bool renderCarried(ItemInHandRenderer& self, BaseActorRenderContext& ctx, ItemContextFlags flags) {
    if (!stash().empty()) pruneStash(false);
    auto* player = currentPlayer();
    auto* data   = player ? lo::client::carry::localData() : nullptr;
    if (!data || !(data->isBlock() || data->isEntity())) {
        if (cache().data) resetCache(true);
        return false;
    }
    auto& c = cache();
    // Rebuilt when the carry changes, and the display actor when the player changed dimension.
    // Also when the client's own copy of the carried entity shows up after the carry sync.
    bool ownCopyArrived = false;
    if (c.data && c.visual.kind == CarryVisual::Kind::Entity && !c.owned.has_value() && !stash().empty()) {
        if (auto tag = carry::parseNbt(data->entity); tag && tag->contains("UniqueID")
                                                      && tag->at("UniqueID").hold(Tag::Int64)) {
            ownCopyArrived = stash().contains(tag->at("UniqueID").get<Int64Tag>().data);
        }
    }
    if (!c.data || !sameVisual(*c.data, *data) || ownCopyArrived
        || (c.entity.has_value() && c.dimension != static_cast<int>(player->getDimensionId()))) {
        rebuild(*data, *player);
    }

    auto const& s   = config::get<carry::CarryClientConfig>().settings;
    auto&       cam = ctx.mScreenContext.camera;

    // Here view * world is the identity and the projection is the hand's (FOV 70), as in Java's
    // RenderHandEvent; vanilla's own hand bobbing lives inside the function we replace.
    glm::mat4 base{1.f};
    if (auto* client = currentClient(); client && client->getOptions().getBobView()) {
        float alpha = ctx.mFrameAlpha;
        auto& ent   = player->getEntityContext();
        float bob   = std::lerp(PlayerMovement::getBobPrev(ent), PlayerMovement::getBob(ent), alpha);
        base        = viewBobTransform(ActorMovement::getPredictedWalkDist(ent, alpha), bob);
    }

    auto kind = DrawStatus::Kind::None;
    switch (c.visual.kind) {
    case CarryVisual::Kind::Block:
    case CarryVisual::Kind::Item: {
        kind = c.visual.kind == CarryVisual::Kind::Block ? DrawStatus::Kind::Block : DrawStatus::Kind::Item;
        if (c.item.isNull()) break;
        auto  ref = cam.worldMatrixStack->push(false);
        auto& m   = *ref.mat->_m;
        m         = m * base * blockTransform(s.facePlayer, c.chest, c.visual);
        self.renderItem(ctx, *player, c.item, false, flags, true, true);
        break;
    }
    case CarryVisual::Kind::Entity: {
        kind        = DrawStatus::Kind::Entity;
        auto* actor = cachedActor();
        if (!actor) break;
        // Drawn in the world pass (drawCarriedEntity): actors drawn from this pass do not show.
        c.entityView = base
                     * entityTransform(
                           carry::entityHeight(*actor),
                           carry::entityWidth(*actor),
                           s.rotateEntitiesSideways,
                           c.visual
                     );
        c.wantEntity = true;
        if (c.owned.has_value()) { // TEMP: the client's own actor, drawn right here
            ActorAtPlayer at(*actor, *player);
            auto const&   ppos = player->getPosition();
            auto          ref  = cam.worldMatrixStack->push(false);
            *ref.mat->_m       = *ref.mat->_m * c.entityView;
            ctx.mEntityRenderDispatcher->render(ctx, *actor, ppos, ppos, Vec2{0.f, 0.f}, false);
        }
        break;
    }
    default:
        break;
    }
    setStatus(kind, c.what, true);
    if (!c.logged) {
        c.logged = true;
        log().info("firstperson: drawing {} {}", carry::carryTypeName(data->type), c.what);
    }
    return true;
}

/// Draws the carried entity in first person. Called from the world's block-entity pass, the pass in
/// which vanilla draws the mob of a spawner (an actor that is not in the level's list); actors drawn
/// from the first-person item pass do not show. The view-space transform of the first-person pass is
/// turned back into a world matrix, with the hand's FOV-70 projection, as for the block.
void drawCarriedEntity(BaseActorRenderContext& ctx) {
    auto& c = cache();
    if (!c.wantEntity || c.visual.kind != CarryVisual::Kind::Entity) return;
    auto* client = currentClient();
    auto* player = currentPlayer();
    if (!client || !player || client->getOptions().getPlayerViewPerspective() != 0) return;
    auto* data = lo::client::carry::localData();
    if (!data || !data->isEntity() || !c.data || !sameVisual(*c.data, *data)) return;
    auto* actor = cachedActor();
    if (!actor) return;

    auto&       cam    = ctx.mScreenContext.camera;
    auto const  view   = *cam.viewMatrixStack->top()._m;
    auto        proj   = cam.projectionMatrixStack->push(false);
    auto&       p      = *proj.mat->_m;
    float       aspect = p[0][0] != 0.f ? p[1][1] / p[0][0] : 16.f / 9.f;
    p[1][1]            = 1.f / std::tan(glm::radians(35.f)); // Java's hand FOV (70)
    p[0][0]            = p[1][1] / aspect;
    auto world         = cam.worldMatrixStack->push(false);
    *world.mat->_m     = glm::inverse(view) * c.entityView;

    ActorAtPlayer at(*actor, *player);
    auto const&   ppos = player->getPosition();
    if (!c.owned.has_value()) ctx.mEntityRenderDispatcher->render(ctx, *actor, ppos, ppos, Vec2{0.f, 0.f}, false);
    { // TEMP diagnostic: plain world placement 3 blocks south-east of the player
        auto w2 = cam.worldMatrixStack->push(false);
        *w2.mat->_m = glm::mat4{1.f};
        auto const& tgt = ctx.mImpl->mCameraTargetPosition.get();
        ctx.mEntityRenderDispatcher->render(ctx, *actor, tgt, ppos + Vec3{2.f, -1.6f, 3.f}, Vec2{0.f, 0.f}, false);
    }
    static bool once = false;
    if (!once) {
        once = true;
        log().info("firstperson: world-pass entity draw, box {}x{}", carry::entityWidth(*actor), carry::entityHeight(*actor));
    }
}

} // namespace

DrawStatus status() {
    std::lock_guard lock(gStatusMutex);
    return gStatus;
}

// ---- hook + module ------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    FirstPersonRenderHook,
    ll::memory::HookPriority::Normal,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderFirstPerson,
    void,
    BaseActorRenderContext& ctx,
    Matrix const&           prevProj,
    ItemContextFlags        flags
) {
    bool handled = false;
    try {
        handled = renderCarried(*this, ctx, flags);
    } catch (std::exception const& e) {
        static bool once = false;
        if (!once) {
            once = true;
            log().error("firstperson: rendering failed: {}", e.what());
        }
    }
    if (!handled) origin(ctx, prevProj, flags);
}

LL_TYPE_INSTANCE_HOOK(
    FirstPersonEntityHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$renderBlockEntities,
    void,
    BaseActorRenderContext& ctx,
    bool                    renderAlphaLayer
) {
    origin(ctx, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        drawCarriedEntity(ctx);
    } catch (std::exception const& e) {
        static bool once = false;
        if (!once) {
            once = true;
            log().error("firstperson: drawing the carried entity failed: {}", e.what());
        }
    }
}

/// Keeps the client's copy of an entity the server removes near the local player for a moment (see
/// `Stashed`) instead of destroying it; unclaimed copies are destroyed after `kStashTime`.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonRemoveActorHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    RemoveActorPacket const& packet
) {
    try {
        auto* client = currentClient();
        auto* level  = client ? client->getLevel() : nullptr;
        auto* player = currentPlayer();
        if (level && player && lo::client::carry::serverHasMod()) {
            ActorUniqueID id    = packet.mEntityId;
            Actor*        actor = level->fetchEntity(id, false);
            if (actor && actor != player && !actor->isPlayer() && actor->hasCategory(ActorCategory::Mob)
                && actor->getPosition().distanceTo(player->getPosition()) <= kStashRadius) {
                auto owner = level->removeEntity(*actor);
                if (owner.has_value()) {
                    auto& slot = stash()[id.rawID];
                    destroyTaken(slot.owner, slot.level);
                    slot.owner.emplace(*owner);
                    slot.level = level;
                    slot.at    = std::chrono::steady_clock::now();
                    return;
                }
            }
        }
    } catch (std::exception const& e) {
        log().error("firstperson: keeping a removed entity failed: {}", e.what());
    }
    origin(source, packet);
}

class FirstPersonModule {
public:
    void install() {
        FirstPersonRenderHook::hook();
        FirstPersonEntityHook::hook();
        FirstPersonRemoveActorHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>(
            [](ll::event::ClientExitLevelEvent&) {
                resetCache(true);
                pruneStash(true);
            }
        );
    }
    void uninstall() {
        FirstPersonRenderHook::unhook();
        FirstPersonEntityHook::unhook();
        FirstPersonRemoveActorHook::unhook();
        if (mExit) ll::event::EventBus::getInstance().removeListener(mExit);
        mExit.reset();
        resetCache(false);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lo::client::firstperson

LO_REGISTER_MODULE(lo::client::firstperson::FirstPersonModule);

#endif // LO_CLIENT
