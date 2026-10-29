#ifdef LO_CLIENT

#include "client/firstperson/FirstPerson.h"

#include <cmath>
#include <exception>
#include <memory>
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
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/deps/nbt/ShortTag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveActorPacketPayload.h"
#include "mc/util/VariantParameterList.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/ActorInitializationMethod.h"
#include "mc/world/actor/DataItem2.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/world/actor/provider/ActorMovement.h"
#include "mc/world/actor/provider/PlayerMovement.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/ActorGarbageCollector.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "client/Client.h"
#include "feature/armpose/StandIn.h"
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
// Everything is drawn in the first-person item pass, after the world, with the hand's projection, like
// Java's RenderHandEvent:
//  - blocks with the item renderer;
//  - entities as a client-only stand-in actor that is never added to the level, drawn through the
//    actor dispatcher; the actor renderer computes the world matrix itself, so
//    `ActorRenderData::applyGlobalAnims` gives it Carry On's full view-space transform (pitch included).
//    The stand-in is built from the synced NBT (`ActorFactory::loadActor`) or, where the client factory
//    refuses that, as a display actor of the type whose look (variant, colours, baby, saddle, ...) is set
//    from the NBT through its synched data. Display actors do not get their size from the definition,
//    so the size comes from the client's own copy of the entity when it was removed for the pickup, or
//    from another entity of the type in the level, or from the NBT's box.
// Threads: these passes run on the render thread; packets and ticks on the client thread. The size
// cache is under `gSizeMutex`.

namespace {

/// The cached render state of the local player's carry (render thread).
struct Cache {
    std::optional<CarryData> data;   ///< the carry the cache was built for
    CarryVisual              visual; ///< its visual
    std::string              what;   ///< item / entity id drawn (selftests, log)
    bool                     chest = false;
    ItemStack                item; ///< block / item visual
    bool                     logged = false;
};

Cache& cache() {
    // Never destroyed: the item stack must not be torn down after the game at process exit.
    static auto* c = new Cache;
    return *c;
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

std::optional<int> intOf(CompoundTag const& tag, char const* key) {
    if (!tag.contains(key)) return std::nullopt;
    auto const& t = tag.at(key);
    if (t.hold(Tag::Byte)) return static_cast<int>(t.get<ByteTag>().data);
    if (t.hold(Tag::Short)) return static_cast<int>(t.get<ShortTag>().data);
    if (t.hold(Tag::Int)) return t.get<IntTag>().data;
    return std::nullopt;
}

std::int64_t uniqueIdOf(CompoundTag const& tag) {
    if (!tag.contains("UniqueID") || !tag.at("UniqueID").hold(Tag::Int64)) return 0;
    return tag.at("UniqueID").get<Int64Tag>().data;
}

// ---- sizes ---------------------------------------------------------------------------------------

struct Size {
    float height = 0.f, width = 0.f;
};
std::mutex gSizeMutex;
/// Box of entities the server removed (unique id -> size), recorded at removal (pickup).
std::unordered_map<std::int64_t, Size>& removedSizes() {
    static auto* m = new std::unordered_map<std::int64_t, Size>;
    return *m;
}
/// Box of adult entities per type, from every entity the client saw removed.
std::unordered_map<std::string, Size>& typeSizes() {
    static auto* m = new std::unordered_map<std::string, Size>;
    return *m;
}

void recordSize(Actor& actor) {
    Size s{carry::entityHeight(actor), carry::entityWidth(actor)};
    if (s.height <= 0.f || s.width <= 0.f) return;
    std::lock_guard lock(gSizeMutex);
    auto&           removed = removedSizes();
    if (removed.size() > 256) removed.clear();
    removed[actor.getOrCreateUniqueID().rawID] = s;
    if (!actor.isBaby()) typeSizes()[actor.getTypeName()] = s;
}

/// Learns the size of every type of entity around (client thread, once a second): a carried entity the
/// client never saw (rejoined while carrying) still gets the right size if one of its type was around.
void learnTypeSizes(Level& level) {
    static int tick = 0;
    if (++tick % 20 != 0) return;
    for (Actor* actor : level.getRuntimeActorList()) {
        if (!actor || actor->isPlayer() || actor->isBaby() || !actor->hasCategory(ActorCategory::Mob)) continue;
        Size s{carry::entityHeight(*actor), carry::entityWidth(*actor)};
        if (s.height <= 0.f || s.width <= 0.f) continue;
        std::lock_guard lock(gSizeMutex);
        typeSizes()[actor->getTypeName()] = s;
    }
}

/// The size of the carried entity (`tag`: its NBT) for Carry On's transform.
Size carriedSize(CompoundTag const& tag, std::string const& id, Actor const& standIn) {
    {
        std::lock_guard lock(gSizeMutex);
        if (auto it = removedSizes().find(uniqueIdOf(tag)); it != removedSizes().end()) return it->second;
    }
    bool baby = intOf(tag, "IsBaby").value_or(0) != 0;
    auto half = [&](Size s) { return baby ? Size{s.height * 0.5f, s.width * 0.5f} : s; };
    {
        std::lock_guard lock(gSizeMutex);
        if (auto it = typeSizes().find(id); it != typeSizes().end()) return half(it->second);
    }
    // Another entity of the type in the level.
    if (auto* level = currentClient() ? currentClient()->getLevel() : nullptr) {
        for (Actor* other : level->getRuntimeActorList()) {
            if (!other || other == &standIn || other->getTypeName() != id) continue;
            Size s{carry::entityHeight(*other), carry::entityWidth(*other)};
            if (s.height <= 0.f || s.width <= 0.f) continue;
            if (!other->isBaby()) {
                std::lock_guard lock(gSizeMutex);
                typeSizes()[id] = s;
            }
            return other->isBaby() == baby ? s : (baby ? half(s) : Size{s.height * 2.f, s.width * 2.f});
        }
    }
    // The stand-in's own box, unless it is the 0.6 x 1.8 default of an actor not initialised from its
    // definition (display actors); then a one-block box.
    Size own{carry::entityHeight(standIn), carry::entityWidth(standIn)};
    bool uninitialised = std::abs(own.width - 0.6f) < 1e-3f && std::abs(own.height - 1.8f) < 1e-3f;
    if (own.height > 0.f && own.width > 0.f && !uninitialised) return own;
    // U13's measurement of the NBT (a temporary actor where the client can load one, else its table of
    // vanilla mob sizes; babies halved); its 0.6 x 1.8 default means "unknown".
    auto box     = lo::feature::armpose::entityBox(carry::toSnbt(tag), currentClient() ? currentClient()->getLevel() : nullptr);
    bool unknown = !box.ok && std::abs(box.width - 0.6f) < 1e-3f && std::abs(box.height - 1.8f) < 1e-3f;
    if (box.width > 0.f && box.height > 0.f && !unknown) return Size{box.height, box.width};
    return half(Size{1.f, 1.f});
}

// ---- stand-in ------------------------------------------------------------------------------------

struct StandIn {
    std::unique_ptr<OwnerPtr<EntityContext>> owner;
    Level*                                   level = nullptr;
    std::string                              snbt;
    Size                                     size;
};
StandIn& standIn() {
    static auto* s = new StandIn;
    return *s;
}

void releaseStandIn() {
    auto& s = standIn();
    if (s.owner) {
        auto* client = currentClient();
        if (s.level && client && client->getLevel() == s.level && s.owner->has_value()) {
            s.level->mActorGarbageCollector->garbageCollectEntity(std::move(*s.owner));
        }
        s.owner.reset();
    }
    s.level = nullptr;
    s.snbt.clear();
}

/// The look of the entity from its NBT (variant, colours, baby, saddle, shorn wool, chest, tamed),
/// applied through the synched data as a SetActorDataPacket from the server would.
void applyLook(Actor& actor, CompoundTag const& tag) {
    std::vector<std::unique_ptr<DataItem>> items;
    auto                                   id = [](ActorDataIDs i) { return static_cast<DataItem::ID>(i); };
    if (auto v = intOf(tag, "Variant")) items.push_back(DataItem::create(id(ActorDataIDs::Variant), int{*v}));
    if (auto v = intOf(tag, "MarkVariant")) items.push_back(DataItem::create(id(ActorDataIDs::MarkVariant), int{*v}));
    if (auto v = intOf(tag, "Color")) {
        items.push_back(DataItem::create(id(ActorDataIDs::ColorIndex), static_cast<schar>(*v)));
    }
    if (auto v = intOf(tag, "Color2")) {
        items.push_back(DataItem::create(id(ActorDataIDs::Color2Index), static_cast<schar>(*v)));
    }
    auto& data  = actor.mEntityData;
    auto  flags = data->getInt64(static_cast<ushort>(ActorDataIDs::Reserved0));
    auto  flag  = [&](char const* key, ActorFlags f) {
        auto v = intOf(tag, key);
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
    if (intOf(tag, "IsBaby").value_or(0) != 0) {
        items.push_back(DataItem::create(id(ActorDataIDs::Reserved038), float{0.5f})); // scale
    }
    data->assignValues(items, actor, std::nullopt);
}

/// The equipment of the entity from its NBT: armour (`Armor`, head to feet then body armour), held item
/// (`Mainhand`) and off-hand item (`Offhand`), each item built from its own tag.
void applyEquipment(Actor& actor, CompoundTag const& tag) {
    auto items = [&](char const* key) {
        std::vector<ItemStack> out;
        if (!tag.contains(key) || !tag.at(key).hold(Tag::List)) return out;
        for (auto const& entry : tag.at(key).get<ListTag>()) {
            if (entry && entry->getId() == Tag::Compound) {
                out.push_back(ItemStack::fromTag(static_cast<CompoundTag const&>(*entry)));
            } else {
                out.emplace_back();
            }
        }
        return out;
    };
    auto armor = items("Armor");
    for (std::size_t i = 0; i < armor.size() && i < static_cast<std::size_t>(SharedTypes::Legacy::ArmorSlot::Count);
         ++i) {
        if (!armor[i].isNull()) {
            log().info("firstperson: equip armour {} {}", i, armor[i].getTypeName());
            actor.setArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(i), armor[i]);
            log().info("firstperson: equip armour done");
        }
    }
    if (auto hand = items("Mainhand"); !hand.empty() && !hand[0].isNull()) {
        log().info("firstperson: (experiment) not equipping hand {}", hand[0].getTypeName());
    }
    if (auto off = items("Offhand"); !off.empty() && !off[0].isNull()) actor.setOffhandSlot(off[0]);
}

/// The stand-in actor for the carried entity `snbt` (built once per carry).
Actor* standInActor(LocalPlayer& player, std::string const& snbt) {
    auto& s      = standIn();
    auto* client = currentClient();
    auto* level  = client ? client->getLevel() : nullptr;
    if (!level) return nullptr;
    if (s.owner && (s.level != level || s.snbt != snbt)) releaseStandIn();
    if (s.owner) return s.owner->tryUnwrap().as_ptr();

    auto tag = carry::parseNbt(snbt);
    auto id  = carry::entityIdOf(snbt);
    if (!tag || id.empty()) return nullptr;
    CompoundTag load = *tag;
    for (auto key :
         {"UniqueID",
          "CustomName",
          "CustomNameVisible",
          "LeasherID",
          "LinksTag",
          "Riding",
          "HurtTime",
          "Motion",
          "Rotation",
          "Pos"}) {
        load.erase(key);
    }
    Vec3 const& at = player.getPosition();
    load["Pos"]    = ListTag{at.x, at.y, at.z};
    DefaultDataLoadHelper helper;
    s.owner = std::make_unique<OwnerPtr<EntityContext>>(
        level->getActorFactory().loadActor(&load, helper, player.getDimension().mHeightRange, nullptr)
    );
    bool loaded = s.owner->tryUnwrap().as_ptr() != nullptr;
    if (!loaded) {
        s.owner = std::make_unique<OwnerPtr<EntityContext>>(
            level->getActorFactory()
                .clientCreateDisplayActor(ActorDefinitionIdentifier(id), &player, at, Vec2{0.f, 0.f})
        );
    }
    Actor* actor = s.owner->tryUnwrap().as_ptr();
    if (!actor) {
        s.owner.reset();
        log().warn("firstperson: cannot build a stand-in for {}", id);
        return nullptr;
    }
    // Lighting and the renderer read the actor's dimension: the player's.
    actor->setDimension(player.mDimension);
    if (!loaded) {
        applyLook(*actor, *tag);
        applyEquipment(*actor, *tag);
    }
    actor->setNameTag("");
    s.level = level;
    s.snbt  = snbt;
    s.size  = carriedSize(*tag, id, *actor);
    log().info(
        "firstperson: stand-in for {} ({}), {} x {}",
        id,
        loaded ? "from the NBT" : "display actor",
        s.size.width,
        s.size.height
    );
    return actor;
}

/// The stand-in while we draw it, and the world matrix it gets (FirstPersonGlobalAnimsHook).
thread_local Actor*    tActor = nullptr;
thread_local glm::mat4 tMatrix{1.f};

void resetCache() {
    auto& c = cache();
    releaseStandIn();
    c.data.reset();
    c.visual = {};
    c.what.clear();
    c.item   = ItemStack{};
    c.chest  = false;
    c.logged = false;
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
        c.what = carry::entityIdOf(c.visual.entity);
        break;
    default:
        break;
    }
}

/// Draws the stand-in with the view-space transform `view`.
bool drawEntity(BaseActorRenderContext& ctx, LocalPlayer& player, glm::mat4 const& view) {
    Actor* actor = standInActor(player, cache().visual.entity);
    if (!actor) return false;
    // Here view * world is the identity: the stack top turns view space into this pass's world space.
    glm::mat4 world = *ctx.mScreenContext.camera.worldMatrixStack->top()._m * view;
    world           = glm::rotate(world, glm::radians(180.f), kY); // Bedrock models face -Z, Java's +Z
    // Model pixels, at the actor's own scale (babies), which the matrix we replace carried.
    float scale = actor->mEntityData->getFloat(static_cast<ushort>(ActorDataIDs::Reserved038));
    if (scale <= 0.f) scale = 1.f;
    world = glm::scale(world, glm::vec3{scale / 16.f});
    // SPEC §4.1: at the carrier's position (light), all rotations zero, hurt time 0.
    actor->moveTo(player.getPosition(), Vec2{0.f, 0.f});
    actor->setYHeadRotations(0.f, 0.f);
    if (actor->hasCategory(ActorCategory::Mob)) static_cast<Mob*>(actor)->setYBodyRotations(0.f, 0.f);
    tActor  = actor;
    tMatrix = world;
    try {
        // No offset of its own on top of our matrix: drawn "at" the camera target.
        Vec3 const target = ctx.mImpl->mCameraTargetPosition;
        ctx.mEntityRenderDispatcher->render(ctx, *actor, target, target, Vec2{0.f, 0.f}, false);
    } catch (...) {
        tActor = nullptr;
        throw;
    }
    tActor = nullptr;
    return true;
}

/// Draws the local player's carry instead of the hand. False: not carrying a block or an entity (the
/// vanilla hand renders; Carry On draws nothing in first person for a carried player).
bool renderCarried(ItemInHandRenderer& self, BaseActorRenderContext& ctx, ItemContextFlags flags) {
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
        kind = DrawStatus::Kind::Entity;
        // Built first so that its size is known.
        if (!standInActor(*player, c.visual.entity)) break;
        auto const& size = standIn().size;
        actorAlive       = drawEntity(
            ctx,
            *player,
            base * entityTransform(size.height, size.width, s.rotateEntitiesSideways, c.visual)
        );
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

/// The actor renderer computes the world matrix of an actor itself; the stand-in gets ours.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonGlobalAnimsHook,
    ll::memory::HookPriority::Normal,
    ActorRenderData,
    &ActorRenderData::applyGlobalAnims,
    void,
    Matrix&                 worldMatrix,
    BaseActorRenderContext& ctx
) {
    origin(worldMatrix, ctx);
    if (tActor && this->mActor == tActor) *worldMatrix._m = tMatrix;
}

/// Records the size of entities the server removes (a pickup removes the carried one): display actors
/// do not get it from their definition.
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
        if (level) {
            Actor* actor = level->fetchEntity(packet.mEntityId, false);
            if (actor && !actor->isPlayer()) recordSize(*actor);
        }
    } catch (std::exception const& e) {
        log().error("firstperson: reading a removed entity failed: {}", e.what());
    }
    origin(source, packet);
}

class FirstPersonModule {
public:
    void install() {
        FirstPersonRenderHook::hook();
        FirstPersonGlobalAnimsHook::hook();
        FirstPersonRemoveActorHook::hook();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(
            [](ll::event::ClientLevelTickEvent& ev) {
                try {
                    learnTypeSizes(ev.level());
                } catch (std::exception const& e) {
                    log().error("firstperson: learning entity sizes failed: {}", e.what());
                }
            }
        );
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>(
            [](ll::event::ClientExitLevelEvent&) {
                std::lock_guard lock(gSizeMutex);
                removedSizes().clear();
            }
        );
    }
    void uninstall() {
        FirstPersonRenderHook::unhook();
        FirstPersonGlobalAnimsHook::unhook();
        FirstPersonRemoveActorHook::unhook();
        if (mExit) ll::event::EventBus::getInstance().removeListener(mExit);
        if (mTick) ll::event::EventBus::getInstance().removeListener(mTick);
        mExit.reset();
        mTick.reset();
    }

private:
    ll::event::ListenerPtr mExit;
    ll::event::ListenerPtr mTick;
};

} // namespace lo::client::firstperson

LO_REGISTER_MODULE(lo::client::firstperson::FirstPersonModule);

#endif // LO_CLIENT
