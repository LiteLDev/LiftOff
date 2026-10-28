// U11: third-person rendering of carried objects (see ThirdPerson.h, docs/SPEC.md §4.1, §4.3).

#ifdef LO_CLIENT

#include "client/thirdperson/ThirdPerson.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/actor/DataDrivenRenderer_tempComponent_HumanoidAdditionalRendering.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/game_refs/OwnerPtr.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "client/Client.h"
#include "client/thirdperson/CarryTransform.h"
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

namespace lo::client::thirdperson {

namespace carry = feature::carry;
using carry::CarryData;
using carry::CarryVisual;
using Clock = std::chrono::steady_clock;

namespace {

/// A carrier not drawn for this long is out of view: its cache entry is dropped.
constexpr auto kOutOfView = std::chrono::seconds(5);

/// What the renderer keeps per carrier (client / render thread).
struct Entry {
    CarryData   data;   ///< the carry the entry was built for
    CarryVisual visual; ///< `visualFor(data)`
    bool        broken = false; ///< nothing drawable (unknown block / entity): skip until the carry changes

    // Block / item: the stack drawn, `isChest` of the carried block, `renderHeight` of the drawn block.
    std::unique_ptr<ItemStack> item;
    bool                       isChest      = false;
    float                      renderHeight = 1.f;

    // Entity: the client-only display actor built from the NBT, its collision box.
    std::unique_ptr<OwnerPtr<EntityContext>> displayActor;
    ClientLevel*                             level  = nullptr;
    float                                    height = 1.f;
    float                                    width  = 1.f;

    Clock::time_point lastDraw;
};

struct State {
    std::mutex                               mutex; ///< entries (render thread vs level events)
    std::unordered_map<std::int64_t, Entry>  entries;
    Clock::time_point                        lastPrune;
    std::mutex                               statsMutex; ///< stats (render thread vs selftests)
    std::unordered_map<std::int64_t, DrawStats> stats;
};
State& state() {
    static State s;
    return s;
}

/// True while this thread draws a carried object (the display actor goes through the hooked dispatcher).
thread_local bool tDrawing = false;
/// The display actor being drawn and the world matrix it must get (see `GlobalAnimsHook`).
thread_local Actor*    tEntityActor = nullptr;
thread_local glm::mat4 tEntityMatrix{1.f};

// ---- helpers -----------------------------------------------------------------------------------

template <class T>
decltype(auto) unwrap(T& storage) {
    if constexpr (requires { storage.get(); }) return storage.get();
    else return (storage);
}

glm::mat4 toGlm(Mat4 const& m) {
    glm::mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) r[c][row] = m.m[c][row];
    return r;
}

bool isChestId(std::string const& id) {
    return id == "minecraft:chest" || id == "minecraft:trapped_chest" || id == "minecraft:ender_chest"
        || id.find("copper_chest") != std::string::npos;
}

bool isMob(Actor const& a) {
    return (static_cast<int>(a.getEntityTypeId()) & static_cast<int>(ActorType::Mob)) == static_cast<int>(ActorType::Mob);
}

// The collision box size (the box itself is not placed: the display actor is never added to the level).
float boxHeight(Actor const& a) { return unwrap(a.mBuiltInComponents->mAABBShapeComponent->mBBDim).y; }
float boxWidth(Actor const& a) { return unwrap(a.mBuiltInComponents->mAABBShapeComponent->mBBDim).x; }

Block const* blockFromSnbt(std::string const& snbt) {
    auto tag = carry::parseNbt(snbt);
    if (!tag) return nullptr;
    auto block = Block::tryGetFromRegistry(*tag);
    return block ? block.as_ptr() : nullptr;
}

/// Height of a block's shape (Carry On: the outline shape; 1 when it has none).
float shapeHeight(Block const& block) {
    AABB        buffer;
    AABB const& box = block.getBlockType().getVisualShape(block, buffer);
    float       h   = box.max.y - box.min.y;
    return h > 0.f ? h : 1.f;
}

/// Removes what the display actor must not have (SPEC §4.1: no name tag, no leash, no links).
void stripForDisplay(CompoundTag& tag) {
    for (auto key : {"UniqueID", "CustomName", "CustomNameVisible", "LeasherID", "LinksTag", "Riding", "HurtTime",
                     "Motion", "Rotation", "Pos"}) {
        tag.erase(key);
    }
}

/// Builds the cache entry of a carry. Never throws.
void build(Entry& e, CarryData const& data, Actor& carrier) {
    e.data         = data;
    e.visual       = carry::visualFor(data);
    e.broken       = false;
    e.item.reset();
    e.displayActor.reset();
    e.isChest      = isChestId(carry::carriedBlockId(data));
    e.renderHeight = 1.f;
    switch (e.visual.kind) {
    case CarryVisual::Kind::Block: {
        Block const* block = blockFromSnbt(e.visual.block);
        if (!block) break;
        // Carry On draws the item of the block's default state; a model override's block state as given.
        Block const* drawn = block;
        if (e.visual.block == data.block) {
            if (Block const* def = block->getBlockType().mDefaultState) drawn = def;
        }
        e.item = std::make_unique<ItemStack>();
        e.item->reinit(*drawn, 1);
        e.renderHeight = e.visual.itemOverride ? 0.5f : shapeHeight(*drawn);
        break;
    }
    case CarryVisual::Kind::Item:
        e.item         = std::make_unique<ItemStack>(game::makeItem(e.visual.item));
        e.renderHeight = 0.5f;
        break;
    case CarryVisual::Kind::Entity: {
        auto* client = currentClient();
        auto* level  = client ? client->getLevel() : nullptr;
        auto  tag    = carry::parseNbt(e.visual.entity);
        auto  id     = carry::entityIdOf(e.visual.entity);
        if (!level || !tag || id.empty()) break;
        stripForDisplay(*tag);
        Vec3 const& at = carrier.getPosition();
        (*tag)["Pos"]  = ListTag{at.x, at.y, at.z};
        // Built from the NBT like a loaded entity (variant, colour, age...), but never added to the level.
        DefaultDataLoadHelper helper;
        auto owner = std::make_unique<OwnerPtr<EntityContext>>(
            level->getActorFactory().loadActor(&*tag, helper, carrier.getDimension().mHeightRange, nullptr)
        );
        Actor* actor = owner->tryUnwrap().as_ptr();
        if (!actor) {
            owner = std::make_unique<OwnerPtr<EntityContext>>(
                level->getActorFactory().clientCreateDisplayActor(ActorDefinitionIdentifier(id), &carrier, at, Vec2{0.f, 0.f})
            );
            actor = owner->tryUnwrap().as_ptr();
        }
        if (!actor) break;
        actor->setNameTag("");
        // Lighting and the renderer read the actor's dimension: the carrier's.
        actor->setDimension(unwrap(carrier.mDimension));
        e.displayActor = std::move(owner);
        e.level        = level;
        e.height       = boxHeight(*actor);
        e.width        = boxWidth(*actor);
        if (e.height <= 0.f) e.height = 1.f;
        if (e.width <= 0.f) e.width = 1.f;
        break;
    }
    case CarryVisual::Kind::None:
        break;
    }
    e.broken = !e.item && !e.displayActor;
    if (e.item && e.item->isNull()) e.broken = true;
    if (e.broken) {
        log().warn("thirdperson: nothing to draw for {}'s carry", carrier.getNameTag());
    } else {
        log().info(
            "thirdperson: drawing {}'s carried {}",
            carrier.getNameTag(),
            e.displayActor ? "entity" : (e.visual.kind == CarryVisual::Kind::Item ? "item" : "block")
        );
    }
}

Actor* displayActorOf(Entry& e) {
    if (!e.displayActor) return nullptr;
    auto* client = currentClient();
    if (!client || client->getLevel() != e.level) return nullptr; // never touch another level's registry
    return e.displayActor->tryUnwrap().as_ptr();
}

/// The camera perspective (0 first person, 1 back, 2 front).
int currentPerspective() {
    auto* client = currentClient();
    if (!client) return -1;
    return client->getOptions().getPlayerViewPerspective();
}

/// The carrier's pose this frame (Carry On reads the same from the player render state).
CarrierPose poseOf(Actor& carrier, Vec3 const& renderPos, Vec2 const& renderRot) {
    CarrierPose p;
    // `renderPos` is the actor position relative to the camera; a player's position is its eye.
    float feetOffset = carrier.getPosition().y - carrier.getAABB().min.y;
    p.feet           = {renderPos.x, renderPos.y - feetOffset, renderPos.z};

    // The rotation the player model is drawn with this frame (interpolated body yaw, pitch). Carry On
    // follows the model's body (the vehicle's rules for riding are already applied by the renderer).
    p.bodyYaw = renderRot.y;
    p.pitch   = renderRot.x;

    bool flying = carrier.isPlayer() && static_cast<Player&>(carrier).isFlying();
    p.sneakCheck = carrier.isSneaking() && !flying;
    p.crouching  = p.sneakCheck;
    p.gliding    = carrier.isGliding();
    p.swimming   = !p.gliding && (carrier.isSwimming() || carrier.getStatusFlag(ActorFlags::Crawling));
    p.inWater    = carrier.isSwimming();
    if (carrier.isPlayer()) p.scale = carry::playerScale(static_cast<Player&>(carrier));
    return p;
}

void recordDraw(std::int64_t id, CarryVisual::Kind kind, bool viaActor, std::array<float, 3> center) {
    auto&            s = state();
    std::scoped_lock lock(s.statsMutex);
    auto&            st = s.stats[id];
    ++st.frames;
    st.kind         = static_cast<int>(kind);
    st.displayActor = viaActor;
    st.center       = center;
}

/// The carried object is the same (ignores the slot, tick and key state, which change nothing drawn).
bool sameCarried(CarryData const& a, CarryData const& b) {
    return a.type == b.type && a.block == b.block && a.blockEntity == b.blockEntity && a.entity == b.entity
        && a.activeScript == b.activeScript && a.scriptRender == b.scriptRender;
}

/// Drops the entries of carriers that are no longer drawn (out of view) or no longer carry (state lock held).
void prune(State& s, Clock::time_point now) {
    if (now - s.lastPrune < std::chrono::seconds(1)) return;
    s.lastPrune = now;
    std::erase_if(s.entries, [&](auto const& kv) {
        return now - kv.second.lastDraw > kOutOfView || !client::carry::carriedOf(kv.first);
    });
}

/// Draws `carrier`'s carry after the carrier itself was drawn.
void drawCarried(BaseActorRenderContext& ctx, Actor& carrier, Vec3 const& cameraTarget, Vec3 const& renderPos, bool identityBase, Vec2 const& rdRot) {
    if (!carrier.isPlayer()) return;
    if (ctx.mIsRenderingOnMap || ctx.mScreenContext.isDrawingUI) return; // paper doll, maps
    // The local player in first person: U10 draws the carry in the hands.
    if (ctx.mScreenContext.isDrawingFirstPersonObjects || (carrier.isLocalPlayer() && currentPerspective() == 0)) return;
    std::int64_t     id   = carrier.getOrCreateUniqueID().rawID;
    CarryData const* data = client::carry::carriedOf(id);

    auto&            s   = state();
    auto             now = Clock::now();
    std::scoped_lock lock(s.mutex);
    prune(s, now);
    if (!data || data->isPlayer()) { // a carried player rides the carrier: nothing to draw
        s.entries.erase(id);
        return;
    }
    auto [it, fresh] = s.entries.try_emplace(id);
    Entry& e         = it->second;
    if (fresh || !sameCarried(e.data, *data)) build(e, *data, carrier);
    e.lastDraw = now;
    if (e.broken) return;

    auto const& settings = config::get<carry::CarryClientConfig>().settings;
    CarrierPose pose     = poseOf(carrier, renderPos, rdRot);
    Mat4        frame    = carrierFrame(pose);

    auto& camera = unwrap(ctx.mScreenContext.camera);
    auto& stack  = unwrap(camera.worldMatrixStack);

    ScriptTransform script{e.visual.translation, e.visual.rotation, e.visual.scale};
    bool            hasScript = !e.data.activeScript.empty();

    if (Actor* actor = displayActorOf(e)) {
        EntityParams params{
            .height    = e.height,
            .width     = e.width,
            .sideways  = settings.rotateEntitiesSideways,
            .hasScript = hasScript,
            .script    = script,
        };
        Mat4 m = frame * entityTransform(pose, params);
        // Carry On: the entity stands at the carrier's position (light), all rotations zero.
        actor->moveTo(carrier.getPosition(), Vec2{0.f, 0.f});
        if (isMob(*actor)) static_cast<Mob*>(actor)->setYBodyRotations(0.f, 0.f);
        {
            // The dispatcher does not take our matrix stack top; the ActorRenderData hook below sets the
            // actor's world matrix (camera-relative, like the stack's identity) for this actor only.
            auto feet = m.apply(0.f, 0.f, 0.f);
            // Bedrock entity geometry faces -Z and is in model pixels (1/16 block); Carry On's entity faces +Z.
            Mat4 world = m;
            world.rotateY(180.f);
            world.scale(1.f / 16.f);
            tEntityActor  = actor;
            tEntityMatrix = toGlm(world);
            tDrawing       = true;
            try {
                unwrap(ctx.mEntityRenderDispatcher)
                    ->render(ctx, *actor, cameraTarget, Vec3{feet[0], feet[1], feet[2]}, Vec2{0.f, 0.f}, false);
            } catch (...) {
                tDrawing     = false;
                tEntityActor = nullptr;
                throw;
            }
            tDrawing     = false;
            tEntityActor = nullptr;
        }
        auto c = m.apply(0.f, e.height / 2.f, 0.f);
        recordDraw(id, CarryVisual::Kind::Entity, true, {c[0] + cameraTarget.x, c[1] + cameraTarget.y, c[2] + cameraTarget.z});
        return;
    }
    if (!e.item) return;

    BlockParams params{
        .isChest      = e.isChest,
        .facePlayer   = settings.facePlayer,
        .renderHeight = e.renderHeight,
        .hasScript    = hasScript,
        .script       = script,
        .backView     = currentPerspective() == 1,
    };
    Mat4 m = frame * blockTransform(pose, params);
    // Bedrock block items are modelled facing south (+Z), like Java's chest item; Java's other block items
    // face north. Turn them so the front ends where Carry On puts it (outward unless facePlayer).
    if (e.visual.kind == CarryVisual::Kind::Block && !e.isChest) m.rotateY(180.f);
    {
        auto ref            = identityBase ? stack.pushIdentity(false) : stack.push(false);
        unwrap(ref.mat->_m) = unwrap(ref.mat->_m) * toGlm(m);
        tDrawing            = true;
        try {
            unwrap(ctx.mItemInHandRenderer)
                .renderItem(ctx, carrier, *e.item, false, ItemContextFlags::WorldPass, true, true);
        } catch (...) {
            tDrawing = false;
            throw;
        }
        tDrawing = false;
    }
    auto c = m.apply(0.f, 0.f, 0.f);
    recordDraw(id, e.visual.kind, false, {c[0] + cameraTarget.x, c[1] + cameraTarget.y, c[2] + cameraTarget.z});
}

} // namespace

// ---- hooks ---------------------------------------------------------------------------------------

namespace {
template <class F>
void guarded(F&& fn) {
    try {
        fn();
    } catch (std::exception const& e) {
        static bool once = false;
        if (!once) log().error("thirdperson: drawing a carried object failed: {}", e.what());
        once = true;
    } catch (...) {
        static bool once = false;
        if (!once) log().error("thirdperson: drawing a carried object failed (native exception)");
        once = true;
    }
}
} // namespace

// The data-driven renderer finishes an actor's world matrix here; our display actor gets Carry On's.
LL_TYPE_INSTANCE_HOOK(
    GlobalAnimsHook,
    ll::memory::HookPriority::Normal,
    ActorRenderData,
    &ActorRenderData::applyGlobalAnims,
    void,
    Matrix&                 worldMatrix,
    BaseActorRenderContext& ctx
) {
    origin(worldMatrix, ctx);
    // Our display actor is drawn with Carry On's transform instead of the matrix the renderer set up.
    if (tEntityActor && unwrap(this->mActor) == tEntityActor) unwrap(worldMatrix._m) = tEntityMatrix;}

LL_TYPE_INSTANCE_HOOK(
    HumanoidRenderHook,
    ll::memory::HookPriority::Normal,
    DataDrivenRenderer_tempComponent_HumanoidAdditionalRendering,
    &DataDrivenRenderer_tempComponent_HumanoidAdditionalRendering::$render,
    void,
    BaseActorRenderContext& ctx,
    ActorRenderData&        rd,
    RenderParams&           params
) {
    origin(ctx, rd, params);
    if (tDrawing) return;
    Actor* actor = unwrap(rd.mActor);
    if (!actor || !actor->isPlayer()) return;
    guarded([&] {
                // The render data holds the world position; the identity world matrix is camera-relative.
        Vec3 const& camera = unwrap(unwrap(ctx.mImpl)->mCameraTargetPosition);
        Vec3 const& pos    = unwrap(rd.position);
        drawCarried(ctx, *actor, camera, Vec3{pos.x - camera.x, pos.y - camera.y, pos.z - camera.z}, true, unwrap(rd.rotation));
    });
}

// ---- selftest API ----------------------------------------------------------------------------------

DrawStats statsOf(std::int64_t uniqueId) {
    auto&            s = state();
    std::scoped_lock lock(s.statsMutex);
    auto             it = s.stats.find(uniqueId);
    return it == s.stats.end() ? DrawStats{} : it->second;
}

void resetStats() {
    auto&            s = state();
    std::scoped_lock lock(s.statsMutex);
    s.stats.clear();
}

bool hasCacheEntry(std::int64_t uniqueId) {
    auto&            s = state();
    std::scoped_lock lock(s.mutex);
    return s.entries.contains(uniqueId);
}

int perspective() { return currentPerspective(); }

void setPerspective(int value) {
    ll::thread::ClientThreadExecutor::getDefault().execute([value] {
        if (auto* client = currentClient()) client->getOptions().setPlayerViewPerspective(value);
    });
}

// ---- module --------------------------------------------------------------------------------------

namespace {
void clearEntries() {
    auto&            s = state();
    std::scoped_lock lock(s.mutex);
    s.entries.clear(); // display actors are destroyed while their level still exists
}
} // namespace

class ThirdPersonModule {
public:
    // The client enables mods at game start, before any level exists.
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { clearEntries(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) { clearEntries(); }));
        // A carry change rebuilds the entry on the next draw; an ended carry drops it now.
        if (!mSyncHooked) {
            mSyncHooked = true;
            client::carry::onSync([](std::int64_t id, CarryData const& data) {
                if (data.isCarrying()) return;
                auto&            s = state();
                std::scoped_lock lock(s.mutex);
                s.entries.erase(id);
            });
        }
        HumanoidRenderHook::hook();
        GlobalAnimsHook::hook();
    }

    void uninstall() {
        HumanoidRenderHook::unhook();
        GlobalAnimsHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        clearEntries();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
    bool                                mSyncHooked = false;
};

} // namespace lo::client::thirdperson

LO_REGISTER_MODULE(lo::client::thirdperson::ThirdPersonModule);

#endif // LO_CLIENT
