#ifdef LO_CLIENT

#include "client/firstperson/FirstPerson.h"

#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/provider/ActorMovement.h"
#include "mc/world/actor/provider/PlayerMovement.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/ActorGarbageCollector.h"
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
    std::optional<CarryData> data;   ///< the carry the cache was built for
    CarryVisual              visual; ///< its visual
    bool                     chest = false;
    ItemStack                item;           ///< block / item visual
    OwnerStorageEntity       entity;         ///< entity visual: a client-only actor (not in the level)
    Level*                   level = nullptr; ///< the level the actor belongs to
    int                      dimension = -1;  ///< the dimension the actor was built in
    bool                     logged = false;
};

Cache& cache() {
    // Never destroyed: the item stack and actor handle must not be torn down after the game at exit.
    static auto* c = new Cache;
    return *c;
}

std::mutex  gStatusMutex;
DrawStatus  gStatus;
bool        gDumped = false; // TEMP: matrix dump for calibration

void setStatus(DrawStatus::Kind kind, std::string what, bool count) {
    std::lock_guard lock(gStatusMutex);
    if (gStatus.kind != kind || gStatus.what != what) {
        gStatus.kind   = kind;
        gStatus.what   = std::move(what);
        gStatus.frames = 0;
    }
    if (count) ++gStatus.frames;
    gStatus.actorAlive = cache().entity.has_value();
}

Actor* cachedActor() {
    auto& c = cache();
    if (!c.entity.has_value()) return nullptr;
    return Actor::tryGetFromEntity(*c.entity, true);
}

/// Releases the display actor. `levelAlive`: the level that owns it still exists (else it is
/// abandoned: its registry is gone with the level).
void releaseActor(bool levelAlive) {
    auto& c = cache();
    if (c.entity.has_value()) {
        if (levelAlive && c.level) {
            try {
                c.level->mActorGarbageCollector->garbageCollectEntity(std::move(c.entity));
            } catch (...) {}
        }
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
    c.item   = ItemStack{};
    c.chest  = false;
    c.logged = false;
    setStatus(DrawStatus::Kind::None, {}, false);
}

std::int64_t randomUniqueId() {
    static std::mt19937_64 rng{std::random_device{}()};
    // Negative ids are never handed out by the server.
    return -static_cast<std::int64_t>((rng() >> 2) | 1);
}

/// A client-only actor built from the carried NBT, not added to the level (it never ticks and is
/// drawn only by us). Rotations zeroed, hurt time 0, no name tag, no leash (SPEC §4.1).
void buildActor(LocalPlayer& player, std::string const& snbt) {
    auto& c     = cache();
    auto* level = currentClient() ? currentClient()->getLevel() : nullptr;
    if (!level) return;
    auto tag = carry::parseNbt(snbt);
    if (!tag) return;
    auto const& pos       = player.getPosition();
    (*tag)["Pos"]         = ListTag{pos.x, pos.y, pos.z};
    (*tag)["Rotation"]    = ListTag{0.f, 0.f};
    (*tag)["Motion"]      = ListTag{0.f, 0.f, 0.f};
    (*tag)["UniqueID"]    = randomUniqueId();
    (*tag)["HurtTime"]    = short{0};
    (*tag)["CustomName"]  = std::string{};
    (*tag)["CustomNameVisible"] = false;
    for (auto key : {"LinksTag", "Leasher", "LeasherID", "Persistent"}) tag->erase(key);
    DefaultDataLoadHelper helper;
    char const* path  = "load";
    auto        owner = level->getActorFactory().loadActor(&*tag, helper, player.getDimension().mHeightRange, nullptr);
    if (owner) {
        c.entity.emplace(*owner); // EntityContext is a handle (registry + id); no game-side copy
    } else {
        path         = "display";
        auto display = level->getActorFactory().clientCreateDisplayActor(
            ActorDefinitionIdentifier(carry::entityIdOf(snbt)), &player, pos, Vec2{0.f, 0.f}
        );
        if (display) c.entity.emplace(*display);
    }
    log().info("firstperson: actor via {}: owner={}", path, c.entity.has_value());
    if (!c.entity) return;
    c.level     = level;
    c.dimension = static_cast<int>(player.getDimensionId());
    auto* actor = cachedActor();
    log().info("firstperson: actor ptr {}", (void*)actor);
    if (actor) {
        if (path[0] == 'l') actor->setDimension(player.mDimension);
        log().info("firstperson: actor dim ok");
        actor->setNameTag("");
        log().info("firstperson: actor ok");
    } else {
        log().info("firstperson: actor missing");
        releaseActor(true);
    }
}

void rebuild(CarryData const& data, LocalPlayer& player) {
    resetCache(true);
    auto& c  = cache();
    c.data   = data;
    c.visual = carry::visualFor(data);
    switch (c.visual.kind) {
    case CarryVisual::Kind::Block: {
        // Carry On draws the item of the block's default state.
        c.chest = isChest(carry::carriedBlockId(CarryData{.type = carry::CarryType::Block, .block = c.visual.block}));
        if (auto tag = carry::parseNbt(c.visual.block)) {
            if (auto state = Block::tryGetFromRegistry(*tag)) {
                auto def = Block::tryGetFromRegistry(HashedString{state->getTypeName()});
                c.item.reinit(def ? *def : *state, 1);
            }
        }
        break;
    }
    case CarryVisual::Kind::Item:
        c.chest = isChest(carry::carriedBlockId(data));
        c.item  = game::makeItem(c.visual.item);
        break;
    case CarryVisual::Kind::Entity:
        buildActor(player, c.visual.entity);
        break;
    default:
        break;
    }
}

std::string describe(Cache const& c) {
    switch (c.visual.kind) {
    case CarryVisual::Kind::Block:
    case CarryVisual::Kind::Item:
        return c.item.isNull() ? std::string{} : c.item.getTypeName();
    case CarryVisual::Kind::Entity:
        return carry::entityIdOf(c.visual.entity);
    default:
        return {};
    }
}

void dumpMatrices(char const* what, BaseActorRenderContext& ctx, Matrix const& prevProj) {
    if (gDumped) return;
    gDumped     = true;
    auto& cam   = ctx.mScreenContext.camera;
    auto  print = [](char const* name, glm::mat4 const& m) {
        log().info(
            "firstperson: {} = [{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}]",
            name, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2],
            m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]
        );
    };
    print("world", *cam.worldMatrixStack->top()._m);
    print("view", *cam.viewMatrixStack->top()._m);
    print("proj", *cam.projectionMatrixStack->top()._m);
    print("prevProj", *prevProj._m);
    log().info("firstperson: {} world={} view={} proj={}", what, cam.worldMatrixStack->stack->size(),
               cam.viewMatrixStack->stack->size(), cam.projectionMatrixStack->stack->size());
}

/// Draws the local player's carry instead of the hand. False: not carrying a block / entity (the
/// vanilla hand renders).
bool renderCarried(ItemInHandRenderer& self, BaseActorRenderContext& ctx, Matrix const& prevProj, ItemContextFlags flags) {
    auto* player = currentPlayer();
    auto* data   = player ? lo::client::carry::localData() : nullptr;
    if (!data || !(data->isBlock() || data->isEntity())) {
        if (cache().data) resetCache(true);
        return false;
    }
    auto& c = cache();
    // Rebuilt when the carry changes, and the display actor when the player changed dimension.
    if (!c.data || *c.data != *data
        || (c.entity.has_value() && c.dimension != static_cast<int>(player->getDimensionId()))) {
        rebuild(*data, *player);
    }

    auto const& s   = config::get<carry::CarryClientConfig>().settings;
    auto&       cam = ctx.mScreenContext.camera;
    dumpMatrices("enter", ctx, prevProj);

    glm::mat4 base{1.f};
    if (auto* client = currentClient(); client && client->getOptions().getBobView()) {
        float alpha = ctx.mFrameAlpha;
        auto& ent   = player->getEntityContext();
        float bob   = std::lerp(PlayerMovement::getBobPrev(ent), PlayerMovement::getBob(ent), alpha);
        base        = viewBobTransform(ActorMovement::getPredictedWalkDist(ent, alpha), bob);
    }

    switch (c.visual.kind) {
    case CarryVisual::Kind::Block:
    case CarryVisual::Kind::Item: {
        if (c.item.isNull()) break;
        auto ref = cam.worldMatrixStack->push(false);
        auto& m  = *ref.mat->_m;
        m        = m * base * blockTransform(s.facePlayer, c.chest, c.visual);
        self.renderItem(ctx, *player, c.item, false, flags, true, true);
        setStatus(c.visual.kind == CarryVisual::Kind::Block ? DrawStatus::Kind::Block : DrawStatus::Kind::Item,
                  describe(c), true);
        break;
    }
    case CarryVisual::Kind::Entity: {
        auto* actor = cachedActor();
        if (!actor) break;
        auto const& ppos = player->getPosition();
        actor->_setPos(ppos);
        float h  = carry::entityHeight(*actor);
        float w  = carry::entityWidth(*actor);
        auto ref = cam.worldMatrixStack->push(false);
        auto& m  = *ref.mat->_m;
        m        = m * base * entityTransform(h, w, s.rotateEntitiesSideways, c.visual);
        ctx.mEntityRenderDispatcher->render(ctx, *actor, ppos, ppos, Vec2{0.f, 0.f}, false);
        setStatus(DrawStatus::Kind::Entity, describe(c), true);
        break;
    }
    default:
        break;
    }
    if (!c.logged) {
        c.logged = true;
        log().info("firstperson: drawing {} {}", carry::carryTypeName(data->type), describe(c));
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
