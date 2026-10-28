#include "feature/armpose/StandIn.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>

#include "ll/api/service/Bedrock.h"

#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AnimateEntityPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/molang/MolangVersion.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/DataItem.h"
#include "mc/world/actor/SynchedActorData.h"
#include "mc/world/actor/player/Player.h"
#include "mc/network/packet/SyncedAttribute.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionHeightRange.h"
#include "mc/world/phys/AABB.h"

#include "core/ArmPose.h"
#include "feature/carry/Capture.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Slowness.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Log.h"

namespace lo::feature::armpose {

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Placement (blocks; tuned against screenshots of the arm pose, see the e2e scenario armpose):
constexpr float kHoldY        = 0.95f; ///< centre of the carried thing above the carrier's feet
constexpr float kSneakDrop    = 0.3f;  ///< lower while sneaking
constexpr float kForward      = 0.55f; ///< centre of a carried block in front of the body
constexpr float kSwimForward  = 1.3f;  ///< swimming / gliding: in front of the head
constexpr float kSwimY        = 0.3f;
constexpr float kBlockScale     = 0.6f; ///< Carry On draws carried blocks at 0.6 in third person
constexpr char  kFallingBlock[] = "minecraft:falling_block";

using DataList = std::vector<std::unique_ptr<DataItem>>;

DataItem* findItem(DataList& list, ActorDataIDs id) {
    for (auto& d : list) {
        if (d && d->getId() == static_cast<DataItem::ID>(id)) return d.get();
    }
    return nullptr;
}

template <class T>
void setItem(DataList& list, ActorDataIDs id, T value) {
    if (auto* d = findItem(list, id); d && d->getType() == DataItem::create(id, value)->getType()) {
        d->setData(std::move(value));
        return;
    }
    std::erase_if(list, [id](auto const& d) { return d && d->getId() == static_cast<DataItem::ID>(id); });
    list.push_back(DataItem::create(id, std::move(value)));
}

std::int64_t flagBit(ActorFlags f) { return std::int64_t{1} << static_cast<int>(f); }

/// Flags of a display actor: never falls, no name tag; `invisible` hides it.
void fixFlags(DataList& list, bool invisible) {
    std::int64_t flags = 0;
    if (auto* d = findItem(list, ActorDataIDs::Reserved0)) {
        if (auto v = d->getData<std::int64_t>()) flags = *v;
    }
    flags &= ~(flagBit(ActorFlags::HasGravity) | flagBit(ActorFlags::CanShowName) | flagBit(ActorFlags::AlwaysShowName));
    if (invisible) flags |= flagBit(ActorFlags::Invisible);
    setItem(list, ActorDataIDs::Reserved0, flags);
}

float scaleOf(DataList& list) {
    if (auto* d = findItem(list, ActorDataIDs::Reserved038)) {
        if (auto v = d->getData<float>()) return *v;
    }
    return 1.0f;
}

template <class T>
std::shared_ptr<T> make(MinecraftPacketIds id) {
    return std::static_pointer_cast<T>(MinecraftPackets::createPacket(id));
}

DimensionHeightRange anyHeightRange() {
    DimensionHeightRange r;
    r.mMin = -64;
    r.mMax = 320;
    return r;
}

/// Loads `snbt` as an actor that is never added to the level and calls `fn(actor)`.
template <class F>
bool withTempActor(std::string_view snbt, Level* level, F&& fn) {
    if (!level) level = ll::service::getLevel().as_ptr();
    if (!level) return false;
    auto tag = carry::parseNbt(snbt);
    if (!tag) return false;
    (*tag)["Pos"] = ListTag{0.0f, 0.0f, 0.0f};
    DefaultDataLoadHelper helper;
    auto ctx = level->getActorFactory().loadActor(&*tag, helper, anyHeightRange(), nullptr);
    if (!ctx) return false;
    Actor* actor = Actor::tryGetFromEntity(*ctx, false);
    if (!actor) return false;
    fn(*actor);
    return true;
}

EntityBox boxOf(Actor& actor) {
    EntityBox b;
    b.width  = carry::entityWidth(actor);
    b.height = carry::entityHeight(actor);
    b.scale  = actor.mScale > 0.0f ? static_cast<float>(actor.mScale) : 1.0f;
    b.type   = actor.getTypeName();
    b.ok     = true;
    return b;
}

std::shared_ptr<Packet> blockAddPacket(std::uint64_t id, Block const& block) {
    auto pkt               = make<AddActorPacket>(MinecraftPacketIds::AddActor);
    pkt->mEntityId->rawID  = static_cast<std::int64_t>(id);
    pkt->mRuntimeId->rawID = id;
    pkt->mActorType        = kFallingBlock;
    auto& list             = *pkt->mData->mData;
    fixFlags(list, false); // no gravity: it never falls on the client
    setItem(list, ActorDataIDs::Variant, static_cast<int>(block.mNetworkId));
    setItem(list, ActorDataIDs::Reserved038, kBlockScale);
    return pkt;
}
std::shared_ptr<Packet> entityAddPacket(std::string_view snbt, std::uint64_t id, StandIn& out, float fixedScale = 0.0f) {
    std::shared_ptr<AddActorPacket> pkt;
    bool ok = withTempActor(snbt, nullptr, [&](Actor& actor) {
        auto box = boxOf(actor);
        AddActorPacketPayload payload(actor);
        pkt                       = make<AddActorPacket>(MinecraftPacketIds::AddActor);
        pkt->mEntityId->rawID     = static_cast<std::int64_t>(id);
        pkt->mRuntimeId->rawID    = id;
        pkt->mActorType           = *payload.mActorType;
        pkt->mAttributes          = *payload.mAttributes;
        pkt->mSynchedProperties->mIntEntries   = payload.mSynchedProperties->mIntEntries;
        pkt->mSynchedProperties->mFloatEntries = payload.mSynchedProperties->mFloatEntries;
        auto& list = *pkt->mData->mData;
        list       = std::move(*payload.mData->mData);
        // Carry On draws the entity without name tag or leash.
        std::erase_if(list, [](auto const& d) {
            return d && (d->getId() == static_cast<DataItem::ID>(ActorDataIDs::Name)
                         || d->getId() == static_cast<DataItem::ID>(ActorDataIDs::LeashHolder));
        });
        fixFlags(list, false);
        // Carry On (third person): 0.6 * 1.666 * (10 - min(9.9, h * w)) * 0.08.
        float s = fixedScale > 0.0f ? fixedScale : 0.6f * 1.666f * (10.0f - std::min(9.9f, box.height * box.width)) * 0.08f;
        float base = scaleOf(list);
        setItem(list, ActorDataIDs::Reserved038, base * s);
        out.scale  = base * s;
        out.height = box.height * s;
        out.width  = box.width * s;
    });
    return ok ? pkt : nullptr;
}

schar angleByte(float degrees) {
    float a = std::fmod(degrees, 360.0f);
    if (a < 0) a += 360.0f;
    return static_cast<schar>(static_cast<int>(std::lround(a / 360.0f * 256.0f)) & 0xFF);
}
} // namespace

namespace {
/// Collision widths / heights of vanilla mobs (Bedrock behavior pack values). A remote client has no
/// behavior definitions, so it cannot measure a temporary actor; it uses these instead.
EntityBox fallbackBox(std::string_view snbt) {
    struct Size {
        char const* id;
        float       w, h;
    };
    static constexpr Size kSizes[] = {
        {"minecraft:pig", 0.9f, 0.9f},           {"minecraft:cow", 0.9f, 1.3f},
        {"minecraft:mooshroom", 0.9f, 1.3f},     {"minecraft:sheep", 0.9f, 1.3f},
        {"minecraft:chicken", 0.6f, 0.8f},       {"minecraft:wolf", 0.6f, 0.8f},
        {"minecraft:cat", 0.6f, 0.7f},           {"minecraft:ocelot", 0.6f, 0.7f},
        {"minecraft:fox", 0.6f, 0.7f},           {"minecraft:rabbit", 0.67f, 0.67f},
        {"minecraft:horse", 1.4f, 1.6f},         {"minecraft:donkey", 1.4f, 1.6f},
        {"minecraft:mule", 1.4f, 1.6f},          {"minecraft:llama", 0.9f, 1.87f},
        {"minecraft:trader_llama", 0.9f, 1.87f}, {"minecraft:goat", 0.9f, 1.3f},
        {"minecraft:bee", 0.55f, 0.5f},          {"minecraft:parrot", 0.5f, 1.0f},
        {"minecraft:turtle", 1.2f, 0.4f},        {"minecraft:axolotl", 0.75f, 0.42f},
        {"minecraft:frog", 0.5f, 0.55f},         {"minecraft:armadillo", 0.7f, 0.65f},
        {"minecraft:bat", 0.5f, 0.9f},           {"minecraft:allay", 0.35f, 0.6f},
        {"minecraft:villager_v2", 0.6f, 1.9f},   {"minecraft:wandering_trader", 0.6f, 1.9f},
        {"minecraft:snow_golem", 0.4f, 1.8f},    {"minecraft:iron_golem", 1.4f, 2.9f},
        {"minecraft:squid", 0.95f, 0.95f},       {"minecraft:glow_squid", 0.95f, 0.95f},
        {"minecraft:dolphin", 0.9f, 0.6f},       {"minecraft:cod", 0.5f, 0.3f},
        {"minecraft:salmon", 0.5f, 0.5f},        {"minecraft:pufferfish", 0.8f, 0.8f},
        {"minecraft:tropicalfish", 0.4f, 0.4f},  {"minecraft:strider", 0.9f, 1.7f},
        {"minecraft:polar_bear", 1.3f, 1.4f},    {"minecraft:panda", 1.7f, 1.5f},
        {"minecraft:camel", 1.7f, 2.375f},       {"minecraft:sniffer", 1.9f, 1.75f},
    };
    EntityBox b;
    auto      tag = carry::parseNbt(snbt);
    b.type        = carry::entityIdOf(snbt);
    for (auto const& s : kSizes) {
        if (b.type == s.id) {
            b.width  = s.w;
            b.height = s.h;
            break;
        }
    }
    if (tag && tag->contains("IsBaby") && (*tag)["IsBaby"].get<ByteTag>().data != 0) {
        b.width  *= 0.5f;
        b.height *= 0.5f;
    }
    return b;
}
} // namespace

EntityBox entityBox(std::string_view snbt, Level* level) {
    // Small LRU per thread (server thread / client thread): carries rarely change, but every
    // selftest / viewer asks again.
    thread_local std::list<std::pair<std::string, EntityBox>> cache;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->first == snbt) {
            cache.splice(cache.begin(), cache, it);
            return cache.front().second;
        }
    }
    EntityBox box;
    withTempActor(snbt, level, [&](Actor& actor) { box = boxOf(actor); });
    if (!box.ok) return fallbackBox(snbt); // not cached: the level may not be ready yet
    cache.emplace_front(std::string(snbt), box);
    if (cache.size() > 16) cache.pop_back();
    return box;
}

StandIn buildStandIn(carry::CarryData const& data, carry::CarryVisual const& visual, std::uint64_t id) {
    using Kind = carry::CarryVisual::Kind;
    StandIn s;
    s.id = id;
    if (data.isPlayer() || !data.isCarrying()) return s; // a carried player rides the carrier (U5)
    switch (visual.kind) {
    case Kind::Block:
    case Kind::Item: {
        // Carry On draws the item of the block's default state; a vanilla client gets that block as a
        // display block (a falling block that never falls). An item visual shows its block, if any.
        Block const* block = nullptr;
        if (visual.kind == Kind::Block) {
            carry::CarryData tmp;
            tmp.type  = carry::CarryType::Block;
            tmp.block = visual.block;
            block     = carry::carriedBlock(tmp);
        } else {
            block = game::blockWith(visual.item);
        }
        if (!block) return s;
        if (auto const* def = game::blockWith(block->getTypeName())) block = def; // default state
        s.kind      = StandIn::Kind::Item;
        s.actorType = kFallingBlock;
        s.item      = block->getTypeName();
        s.scale     = kBlockScale;
        s.height    = kBlockScale;
        s.width     = kBlockScale;
        // Built from a real (temporary) falling block, so the client gets every data item it expects.
        CompoundTag nbt;
        nbt["identifier"]   = StringTag("minecraft:falling_block");
        nbt["FallingBlock"] = std::move(*block->mSerializationId->clone());
        nbt["Variant"]      = IntTag(static_cast<int>(block->mNetworkId));
        nbt["Time"]         = ByteTag(1);
        StandIn tmp;
        s.add = entityAddPacket(carry::toSnbt(nbt), id, tmp, kBlockScale);
        if (!s.add) s.add = blockAddPacket(id, *block);
        break;
    }
    case Kind::Entity: {
        s.add = entityAddPacket(visual.entity, id, s);
        if (!s.add) {
            log().warn("armpose: cannot build the stand-in of {}", carry::entityIdOf(visual.entity));
            return s;
        }
        s.kind      = StandIn::Kind::Entity;
        s.actorType = *static_cast<AddActorPacket&>(*s.add).mActorType;
        break;
    }
    case Kind::None: break;
    }
    return s;
}

Placement placementFor(StandIn const& standIn, Player const& carrier) {
    Placement p;
    p.yaw         = carrier.getInterpolatedBodyRot(0.0f);
    float rad     = p.yaw * kPi / 180.0f;
    Vec3  forward = {-std::sin(rad), 0.0f, std::cos(rad)};
    Vec3  feet    = carrier.getFeetPos();
    bool  lying   = carrier.isSwimming() || carrier.isGliding(); // as the animations
    float centreY = lying ? kSwimY : kHoldY - (carrier.isSneaking() ? kSneakDrop : 0.0f);
    float ahead   = lying ? kSwimForward : kForward;
    float bottom  = 0.0f; // stand-in feet relative to the centre
    if (standIn.kind == StandIn::Kind::Item) {
        bottom = -standIn.height * 0.5f; // a falling block stands on its bottom centre
    } else {
        bottom = -standIn.height * 0.5f;
        if (!lying) ahead = std::max(kForward, 0.25f + standIn.width * 0.5f);
    }
    p.pos = Vec3{feet.x + forward.x * ahead, feet.y + centreY + bottom, feet.z + forward.z * ahead};
    return p;
}

void moveAddPacket(StandIn const& standIn, Placement const& at) {
    if (!standIn.add) return;
    auto& pkt           = static_cast<AddActorPacket&>(*standIn.add);
    pkt.mPos            = at.pos;
    pkt.mRot            = Vec2{0.0f, at.yaw};
    pkt.mYHeadRotation  = at.yaw;
    pkt.mYBodyRotation  = at.yaw;
}

std::shared_ptr<Packet> movePacket(StandIn const& standIn, Placement const& at) {
    auto  pkt             = make<MoveActorAbsolutePacket>(MinecraftPacketIds::MoveAbsoluteActor);
    auto& d               = *pkt->mMoveData;
    d.mRuntimeId->rawID   = standIn.id;
    d.mHeader->mRaw       = 0;
    d.mPos                = at.pos;
    d.mRotX               = 0;
    d.mRotY               = angleByte(at.yaw);
    d.mRotYHead           = angleByte(at.yaw);
    d.mRotYBody           = angleByte(at.yaw);
    return pkt;
}

std::shared_ptr<Packet> removePacket(std::uint64_t id) {
    auto pkt              = make<RemoveActorPacket>(MinecraftPacketIds::RemoveActor);
    pkt->mEntityId->rawID = static_cast<std::int64_t>(id);
    return pkt;
}

std::shared_ptr<Packet> animatePacket(Player const& carrier, std::string const& animation, std::string const& controller) {
    auto pkt = make<AnimateEntityPacket>(MinecraftPacketIds::TriggerAnimation);
    pkt->mRuntimeIds->clear();
    pkt->mRuntimeIds->push_back(carrier.getRuntimeID());
    pkt->mAnimation             = animation;
    pkt->mNextState             = "";
    bool none                   = animation == core::armpose::kNoneAnimation;
    pkt->mStopExpression        = none ? "1" : "0";
    pkt->mStopExpressionVersion = MolangVersion::Latest;
    pkt->mController            = controller;
    pkt->mBlendOutTime          = 0.0f;
    return pkt;
}

std::uint64_t nextStandInId() {
    static std::atomic<std::uint64_t> next{0};
    return 0x4C4F000000000000ull + ++next;
}

} // namespace lo::feature::armpose
