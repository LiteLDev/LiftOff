#ifdef LO_CLIENT

#include "client/firstperson/FirstPerson.h"

#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

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
#include "mc/world/level/block/Block.h"
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

/// The cached render state of the local player's carry.
struct Cache {
    std::optional<CarryData>     data;   ///< the carry the cache was built for
    CarryVisual                  visual; ///< its visual
    std::string                  what;   ///< item / entity id drawn (selftests, log)
    bool                         chest = false;
    ItemStack                    item;                ///< block / item visual
    std::optional<WeakEntityRef> entity;              ///< entity visual: a client-only display actor
    Level*                       level     = nullptr; ///< the level that owns the display actor
    int                          dimension = -1;      ///< the dimension the display actor lives in
    bool                         logged    = false;
};

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
    return c.entity->tryUnwrap<Actor>().as_ptr();
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
    if (c.entity.has_value()) {
        if (levelAlive && c.level && cachedActor()) c.level->removeDisplayEntity(*c.entity);
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
    c.chest  = false;
    c.logged = false;
    setStatus(DrawStatus::Kind::None, {}, false);
}

/// Zeroes every rotation of the display actor (SPEC §4.1).
void zeroRotations(Actor& actor) {
    actor.setRotationWrapped(Vec2{0.f, 0.f});
    actor.setYHeadRotations(0.f, 0.f);
    if (actor.hasCategory(ActorCategory::Mob)) static_cast<Mob&>(actor).setYBodyRotations(0.f, 0.f);
}

/// A client-only display actor of the carried entity's type (SPEC §4.1): rotations zeroed, hurt time
/// 0, no name tag, no leash. It waits above the build limit and is drawn only by us.
/// Limitation: the rest of the carried NBT (variant, colour, age...) is not applied; `Actor::load`
/// on a client display actor crashes the game, and the client factory's `loadActor` refuses.
void buildActor(LocalPlayer& player, std::string const& snbt) {
    auto& c     = cache();
    auto* level = currentClient() ? currentClient()->getLevel() : nullptr;
    if (!level) return;
    auto id = carry::entityIdOf(snbt);
    if (id.empty()) return;
    auto park = parkPos(player);
    auto owner =
        level->getActorFactory().clientCreateDisplayActor(ActorDefinitionIdentifier(id), &player, park, Vec2{0.f, 0.f});
    if (!owner) {
        log().warn("firstperson: cannot create a display actor for {}", id);
        return;
    }
    WeakEntityRef weak  = owner->getWeakRef();
    auto*         actor = level->addDisplayEntity(player.getDimensionBlockSource(), std::move(owner));
    if (!actor) return;
    c.entity    = weak;
    c.level     = level;
    c.dimension = static_cast<int>(player.getDimensionId());
    actor->setNameTag("");
    zeroRotations(*actor);
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
        actor._setPos(player.getPosition());
        zeroRotations(actor);
    }
    ~ActorAtPlayer() { actor._setPos(parkPos(player)); }
    ActorAtPlayer(ActorAtPlayer const&)            = delete;
    ActorAtPlayer& operator=(ActorAtPlayer const&) = delete;
};

/// Draws the local player's carry instead of the hand. False: not carrying a block or an entity (the
/// vanilla hand renders; Carry On draws nothing in first person for a carried player).
bool renderCarried(ItemInHandRenderer& self, BaseActorRenderContext& ctx, ItemContextFlags flags) {
    auto* player = currentPlayer();
    auto* data   = player ? lo::client::carry::localData() : nullptr;
    if (!data || !(data->isBlock() || data->isEntity())) {
        if (cache().data) resetCache(true);
        return false;
    }
    auto& c = cache();
    // Rebuilt when the carry changes, and the display actor when the player changed dimension.
    if (!c.data || !sameVisual(*c.data, *data)
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
        ActorAtPlayer at(*actor, *player);
        auto const&   ppos = player->getPosition();
        float         h    = carry::entityHeight(*actor);
        float         w    = carry::entityWidth(*actor);
        auto          ref  = cam.worldMatrixStack->push(false);
        auto&         m    = *ref.mat->_m;
        m                  = m * base * entityTransform(h, w, s.rotateEntitiesSideways, c.visual);
        ctx.mEntityRenderDispatcher->render(ctx, *actor, ppos, ppos, Vec2{0.f, 0.f}, false);
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

class FirstPersonModule {
public:
    void install() {
        FirstPersonRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>(
            [](ll::event::ClientExitLevelEvent&) { resetCache(true); }
        );
    }
    void uninstall() {
        FirstPersonRenderHook::unhook();
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
