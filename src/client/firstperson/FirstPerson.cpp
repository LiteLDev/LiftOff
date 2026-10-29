#ifdef LO_CLIENT

#include "client/firstperson/FirstPerson.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "glm/gtc/constants.hpp"
#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderer.h"
#include "mc/client/renderer/actor/v2/DataDrivenRendererV2.h"
#include "mc/client/renderer/actor/v2/GameplayRelatedRenderData.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveActorPacketPayload.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/provider/ActorMovement.h"
#include "mc/world/actor/provider/PlayerMovement.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/ActorGarbageCollector.h"
#include "mc/world/level/block/Block.h"

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

// ---- renderer state ------------------------------------------------------------------------------
//
// Blocks are drawn in the first-person item pass with the item renderer, exactly where Java draws
// them. Entities cannot be drawn from that pass (the actor renderer draws nothing for an actor outside
// the level's entity pass), so the carried entity is the client's own copy of it, kept in the level:
//  - when the server removes an entity near the local player, the client keeps it for a moment instead
//    of deleting it (`kept`); if the local player's carry turns out to be that entity, it stays as the
//    carried visual (fully initialised, with the variant, colours, age and equipment the server sent);
//    otherwise it is deleted as the server asked;
//  - the level draws it in its normal entity pass, and `ActorRenderData::applyGlobalAnims` gives it the
//    first-person world matrix (Carry On's view-space transform turned by the camera) instead of its own;
//    in the third-person views it is hidden (U11 draws the carry there);
//  - when the carry ends it is deleted.
// Threads: the item pass and the entity pass run on the render thread; packets, ticks and level edits on
// the client thread. The shared state is under `gMutex`.

namespace {

/// The cached render state of the local player's carry (render thread).
struct Cache {
    std::optional<CarryData> data;         ///< the carry the cache was built for
    CarryVisual              visual;       ///< its visual
    std::string              what;         ///< item / entity id drawn (selftests, log)
    std::int64_t             entityId = 0; ///< entity visual: unique id of the carried entity (NBT)
    bool                     chest    = false;
    ItemStack                item; ///< block / item visual
    bool                     logged = false;
};

Cache& cache() {
    // Never destroyed: the item stack must not be torn down after the game at process exit.
    static auto* c = new Cache;
    return *c;
}

constexpr auto  kKeepTime   = std::chrono::seconds(3);
constexpr float kKeepRadius = 64.f;
/// The carried entity is drawn this many times farther and bigger than Carry On's transform says: the
/// same picture, but out of the range in which the game fades mobs next to the camera.
constexpr float kDepthFactor = 2.5f;

/// State shared by the render and client threads.
struct Shared {
    std::mutex mutex;
    /// Entities the server removed near the local player that the client kept (unique id -> when).
    std::unordered_map<std::int64_t, std::chrono::steady_clock::time_point> kept;
    std::int64_t              carried = 0;   ///< the kept entity that is the local player's carried visual (0: none)
    glm::vec3                 feetView{0.f}; ///< where its feet are in view space (Carry On's transform)
    float                     scale       = 1.f; ///< its size relative to normal
    float                     yawOffset   = 0.f; ///< its yaw relative to the camera (sideways, script)
    float                     baseScale   = 0.f; ///< its own scale (baby...), read when it became the carry
    float                     baseHeight  = 0.f; ///< its box before we scaled it
    float                     baseWidth   = 0.f;
    bool                      firstPerson = false; ///< drawn in first person this frame
    std::vector<std::int64_t> remove;              ///< kept entities to delete now (client thread)
};
Shared& shared() {
    static auto* s = new Shared;
    return *s;
}

std::mutex gStatusMutex;
DrawStatus gStatus;

void setStatus(DrawStatus::Kind kind, std::string const& what, bool count, bool actorAlive) {
    std::lock_guard lock(gStatusMutex);
    if (gStatus.kind != kind || gStatus.what != what) {
        gStatus.kind   = kind;
        gStatus.what   = what;
        gStatus.frames = 0;
    }
    if (count) ++gStatus.frames;
    gStatus.actorAlive = actorAlive;
}

/// The fields of a carry that decide what is drawn (a re-sync that only refreshes the hotbar slot,
/// tick or key state keeps the cache).
bool sameVisual(CarryData const& a, CarryData const& b) {
    return a.type == b.type && a.block == b.block && a.blockEntity == b.blockEntity && a.entity == b.entity
        && a.player == b.player && a.activeScript == b.activeScript && a.scriptRender == b.scriptRender;
}

std::int64_t uniqueIdOf(std::string const& snbt) {
    auto tag = carry::parseNbt(snbt);
    if (!tag || !tag->contains("UniqueID") || !tag->at("UniqueID").hold(Tag::Int64)) return 0;
    return tag->at("UniqueID").get<Int64Tag>().data;
}

/// Lets go of the carried entity (deleted on the next client tick).
void dropCarriedEntity() {
    auto&           s = shared();
    std::lock_guard lock(s.mutex);
    if (s.carried) s.remove.push_back(s.carried);
    s.carried     = 0;
    s.firstPerson = false;
    s.baseScale   = 0.f;
    s.baseHeight  = 0.f;
    s.baseWidth   = 0.f;
}

void resetCache() {
    auto& c = cache();
    dropCarriedEntity();
    c.data.reset();
    c.visual = {};
    c.what.clear();
    c.entityId = 0;
    c.item     = ItemStack{};
    c.chest    = false;
    c.logged   = false;
    setStatus(DrawStatus::Kind::None, {}, false, false);
}

void rebuild(CarryData const& data) {
    resetCache();
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
        c.what     = carry::entityIdOf(c.visual.entity);
        c.entityId = uniqueIdOf(c.visual.entity);
        break;
    default:
        break;
    }
}

/// Draws the local player's carry instead of the hand. False: not carrying a block or an entity (the
/// vanilla hand renders; Carry On draws nothing in first person for a carried player).
bool renderCarried(
    ItemInHandRenderer&     self,
    BaseActorRenderContext& ctx,
    Matrix const&           worldProj,
    ItemContextFlags        flags
) {
    auto* player = currentPlayer();
    auto  copy   = player ? lo::client::carry::localCopy() : std::nullopt; // locked copy (render thread)
    auto* data   = copy ? &*copy : nullptr;
    if (!data || !(data->isBlock() || data->isEntity())) {
        if (cache().data) resetCache();
        return false;
    }
    auto& c = cache();
    if (!c.data || !sameVisual(*c.data, *data)) rebuild(*data);

    auto const& s   = config::get<carry::CarryClientConfig>().settings;
    auto&       cam = ctx.mScreenContext.camera;

    // Here view * world is the identity and the projection is the hand's (FOV 70), as in Java's
    // RenderHandEvent; vanilla's own hand bobbing lives inside the function we replace.
    float     alpha = ctx.mFrameAlpha;
    glm::mat4 base{1.f};
    if (auto* client = currentClient(); client && client->getOptions().getBobView()) {
        auto& ent = player->getEntityContext();
        float bob = std::lerp(PlayerMovement::getBobPrev(ent), PlayerMovement::getBob(ent), alpha);
        base      = viewBobTransform(ActorMovement::getPredictedWalkDist(ent, alpha), bob);
    }

    auto kind       = DrawStatus::Kind::None;
    bool actorAlive = false;
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
        kind                  = DrawStatus::Kind::Entity;
        auto*           level = currentClient() ? currentClient()->getLevel() : nullptr;
        auto&           sh    = shared();
        std::lock_guard lock(sh.mutex);
        if (!sh.carried && c.entityId && sh.kept.contains(c.entityId)) {
            sh.kept.erase(c.entityId);
            sh.carried = c.entityId;
            log().info("firstperson: carrying the client's own {}", c.what);
        }
        Actor* actor = sh.carried && level ? level->fetchEntity(ActorUniqueID{sh.carried}, false) : nullptr;
        if (!actor) {
            sh.firstPerson = false;
            break;
        }
        actorAlive = true;
        // Its size before we scale it (the box follows the scale).
        if (sh.baseHeight <= 0.f) {
            sh.baseHeight = carry::entityHeight(*actor);
            sh.baseWidth  = carry::entityWidth(*actor);
        }
        glm::mat4 view = base * entityTransform(sh.baseHeight, sh.baseWidth, s.rotateEntitiesSideways, c.visual);
        // The entity is drawn by the level's entity pass with the world projection: scale x / y so it
        // lands where the hand projection (`proj`, FOV 70 as in Java) would put it, and its size with it.
        auto const& hand = *cam.projectionMatrixStack->top()._m;
        auto const& wp   = *worldProj._m;
        float       kx = 1.f, ky = 1.f;
        if (wp[0][0] != 0.f && wp[1][1] != 0.f) {
            kx = hand[0][0] / wp[0][0];
            ky = hand[1][1] / wp[1][1];
        }
        glm::vec4 feet = view * glm::vec4{0.f, 0.f, 0.f, 1.f};
        sh.feetView    = glm::vec3{feet.x * kx, feet.y * ky, feet.z};
        sh.scale       = 0.8f * 0.5f * (kx + ky) * (c.visual.scale[0] + c.visual.scale[1] + c.visual.scale[2]) / 3.f;
        sh.yawOffset   = c.visual.rotation[1] + (s.rotateEntitiesSideways ? 90.f : 0.f);
        sh.firstPerson = true;
        break;
    }
    default:
        break;
    }
    setStatus(kind, c.what, kind != DrawStatus::Kind::Entity || actorAlive, actorAlive);
    if (!c.logged) {
        c.logged = true;
        log().info("firstperson: drawing {} {}", carry::carryTypeName(data->type), c.what);
    }
    return true;
}

/// Client tick: deletes kept entities nobody claimed (or that were let go), and keeps the carried one
/// next to the player (light, culling) with no rotation, no name tag.
void clientTick(Level& level) {
    auto*                     player = currentPlayer();
    auto&                     sh     = shared();
    std::vector<std::int64_t> remove;
    std::int64_t              carried = 0;
    {
        std::lock_guard lock(sh.mutex);
        auto            now = std::chrono::steady_clock::now();
        std::erase_if(sh.kept, [&](auto const& kv) {
            if (now - kv.second < kKeepTime) return false;
            remove.push_back(kv.first);
            return true;
        });
        remove.insert(remove.end(), sh.remove.begin(), sh.remove.end());
        sh.remove.clear();
        carried = sh.carried;
    }
    for (auto id : remove) {
        if (auto* actor = level.fetchEntity(ActorUniqueID{id}, false)) {
            auto owner = level.removeEntity(*actor);
            if (owner.has_value()) level.mActorGarbageCollector->garbageCollectEntity(std::move(owner));
        }
    }
    if (carried && player) {
        // Placed precisely every frame by placeCarried, which the entity pass only calls for actors in
        // view: keep it in front of the player between frames.
        if (auto* actor = level.fetchEntity(ActorUniqueID{carried}, false)) {
            float yaw = glm::radians(player->getRotation().y);
            Vec3  at  = player->getPosition() + Vec3{-std::sin(yaw) * 1.2f, -0.9f, std::cos(yaw) * 1.2f};
            actor->moveTo(at, Vec2{0.f, player->getRotation().y});
            if (!actor->getNameTag().empty()) actor->setNameTag("");
        }
    }
}

/// Forget everything of a level that is going away (its actors go with it).
void clearShared() {
    auto&           sh = shared();
    std::lock_guard lock(sh.mutex);
    sh.kept.clear();
    sh.remove.clear();
    sh.carried     = 0;
    sh.firstPerson = false;
}

} // namespace

DrawStatus status() {
    std::lock_guard lock(gStatusMutex);
    return gStatus;
}

// ---- hooks + module -----------------------------------------------------------------------------

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
        handled = renderCarried(*this, ctx, prevProj, flags);
    } catch (std::exception const& e) {
        static bool once = false;
        if (!once) {
            once = true;
            log().error("firstperson: rendering failed: {}", e.what());
        }
    }
    if (!handled) origin(ctx, prevProj, flags);
}

namespace {
/// True for the kept entity that is the local carried visual.
bool isCarriedActor(Actor const& actor) {
    auto& sh = shared();
    if (!sh.carried) return false; // unlocked fast path; checked again under the lock
    std::lock_guard lock(sh.mutex);
    return sh.carried && const_cast<Actor&>(actor).getOrCreateUniqueID().rawID == sh.carried;
}

/// Puts the carried entity where Carry On's first-person transform puts it: its feet at the view-space
/// position turned into the world by the camera, facing away from the camera, scaled. Previous and
/// current values follow the player's, so the frame interpolation keeps it fixed on the screen.
/// False: not drawn now (third-person views: U11 draws the carry there).
bool placeCarried(Actor& actor) {
    auto* player = currentPlayer();
    auto* client = currentClient();
    if (!player || !client) return false;
    auto&           sh = shared();
    std::lock_guard lock(sh.mutex);
    if (!sh.firstPerson || client->getOptions().getPlayerViewPerspective() != 0) return false;
    if (sh.baseScale <= 0.f) sh.baseScale = actor.mScale > 0.f ? static_cast<float>(actor.mScale) : 1.f;

    auto offset = [&](Vec2 const& rot) {
        glm::mat4 cam = glm::rotate(glm::mat4{1.f}, glm::radians(180.f - rot.y), kY); // yaw 0 looks south (+Z)
        cam           = glm::rotate(cam, glm::radians(-rot.x), kX);                   // positive pitch looks down
        glm::vec4 w   = cam * glm::vec4{sh.feetView * kDepthFactor, 1.f};
        return Vec3{w.x, w.y, w.z};
    };
    auto& pv    = *player->mBuiltInComponents->mStateVectorComponent;
    auto& pr    = *player->mBuiltInComponents->mActorRotationComponent;
    auto& av    = *actor.mBuiltInComponents->mStateVectorComponent;
    auto& ar    = *actor.mBuiltInComponents->mActorRotationComponent;
    av.mPos     = pv.mPos.get() + offset(pr.mRot);
    av.mPosPrev = pv.mPosPrev.get() + offset(pr.mRotPrev);
    float yaw = pr.mRot->y + sh.yawOffset, yawPrev = pr.mRotPrev->y + sh.yawOffset;
    ar.mRot     = Vec2{0.f, yaw};
    ar.mRotPrev = Vec2{0.f, yawPrev};
    actor.setYHeadRotations(yaw, yawPrev);
    if (actor.hasCategory(ActorCategory::Mob)) static_cast<Mob&>(actor).setYBodyRotations(yaw, yawPrev);
    actor.mScale     = sh.baseScale * sh.scale * kDepthFactor;
    actor.mScalePrev = sh.baseScale * sh.scale * kDepthFactor;
    static bool once = false;
    if (!once) {
        once = true;
        log().info(
            "firstperson: entity placed, feet ({}, {}, {}) in view, scale {} x {}",
            sh.feetView.x,
            sh.feetView.y,
            sh.feetView.z,
            sh.baseScale,
            sh.scale
        );
    }
    return true;
}
} // namespace

/// The level's entity pass (data-driven renderer v2) queues every visible actor here; the carried
/// entity is placed for the first-person view first, or left out.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonQueueHook,
    ll::memory::HookPriority::Normal,
    DataDrivenRendererV2,
    &DataDrivenRendererV2::tryAddActor,
    bool,
    Actor&                                actor,
    std::shared_ptr<ActorRenderer> const& renderer,
    ShadowContext                         shadowContext
) {
    if (isCarriedActor(actor)) {
        bool draw = false;
        try {
            draw = placeCarried(actor);
        } catch (std::exception const& e) {
            static bool once = false;
            if (!once) {
                once = true;
                log().error("firstperson: placing the carried entity failed: {}", e.what());
            }
        }
        if (!draw) return false;
    }
    return origin(actor, renderer, shadowContext);
}

/// Mobs next to the camera fade out; the carried one is always next to it: no fading while the entity
/// pass builds a frame with it in first person.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonFrameHook,
    ll::memory::HookPriority::Normal,
    DataDrivenRendererV2,
    &DataDrivenRendererV2::generateFrame,
    uint,
    GameplayRelatedRenderData const& data
) {
    BaseActorRenderContext* ctx = data.mContext;
    bool                    fp  = false;
    {
        auto&           sh = shared();
        std::lock_guard lock(sh.mutex);
        fp = sh.carried && sh.firstPerson;
    }
    if (!ctx || !fp) return origin(data);
    float dither                           = ctx->mActorDitheringMaxDistance;
    bool  meshDither                       = ctx->mScreenContext.isDitheringEnabled;
    ctx->mActorDitheringMaxDistance        = 0.f;
    ctx->mScreenContext.isDitheringEnabled = false;
    static bool once                       = false;
    if (!once) {
        once = true;
        log().info("firstperson: entity frame dithering distance {}, mesh dithering {}", dither, meshDither);
    }
    uint result                            = origin(data);
    ctx->mActorDitheringMaxDistance        = dither;
    ctx->mScreenContext.isDitheringEnabled = meshDither;
    return result;
}

/// Mobs next to the camera fade out (dithering); the carried one is always next to it.
LL_TYPE_STATIC_HOOK(
    FirstPersonDitherHook,
    ll::memory::HookPriority::Normal,
    ActorRenderer,
    &ActorRenderer::isDitheringEnabled,
    bool,
    BaseActorRenderContext const& ctx,
    Actor const&                  actor
) {
    if (isCarriedActor(actor)) return false;
    return origin(ctx, actor);
}

/// Keeps the client's copy of an entity the server removes near the local player for a moment (see
/// the comment at the top of the renderer state) instead of deleting it at once.
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
                && actor->getPosition().distanceTo(player->getPosition()) <= kKeepRadius) {
                auto&           sh = shared();
                std::lock_guard lock(sh.mutex);
                sh.kept[id.rawID] = std::chrono::steady_clock::now();
                return;
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
        FirstPersonDitherHook::hook();
        FirstPersonQueueHook::hook();
        FirstPersonFrameHook::hook();
        FirstPersonRemoveActorHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) {
            clearShared();
        }));
        mListeners.push_back(
            bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent& ev) {
                try {
                    clientTick(ev.level());
                } catch (std::exception const& e) {
                    log().error("firstperson: client tick failed: {}", e.what());
                }
            })
        );
    }
    void uninstall() {
        FirstPersonRenderHook::unhook();
        FirstPersonDitherHook::unhook();
        FirstPersonQueueHook::unhook();
        FirstPersonFrameHook::unhook();
        FirstPersonRemoveActorHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        clearShared();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lo::client::firstperson

LO_REGISTER_MODULE(lo::client::firstperson::FirstPersonModule);

#endif // LO_CLIENT
