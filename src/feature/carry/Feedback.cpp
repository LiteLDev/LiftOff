#include "feature/carry/Feedback.h"

#include <memory>
#include <string>

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/LevelSoundEvent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorSwingSource.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "game/Blocks.h"

namespace lo::feature::carry {

namespace {
/// A packet made by the game's own factory. `AnimatePacket pkt;` / `PlaySoundPacket pkt;` leave
/// `mSerializationMode` uninitialised (no constructor sets it), and the garbage mode makes the
/// packet unreadable: real clients are disconnected ("An error has occurred").
template <class T>
std::shared_ptr<T> makePacket(MinecraftPacketIds id) {
    return std::static_pointer_cast<T>(MinecraftPackets::createPacket(id));
}
} // namespace

void playBlockSound(Player& player, BlockPos const& pos, Block const& block, BlockSound which) {
    auto type = which == BlockSound::Hit ? SharedTypes::Legacy::LevelSoundEvent::Hit
                                         : SharedTypes::Legacy::LevelSoundEvent::Place;
    player.playSound(player.getDimensionBlockSourceConst(), type, pos.center(), static_cast<int>(block.mNetworkId));
}

void playSound(int dimension, Vec3 const& pos, std::string_view name, float volume, float pitch) {
    auto  ptr = makePacket<PlaySoundPacket>(MinecraftPacketIds::PlaySound);
    auto& pkt = *ptr;
    pkt.mName                     = std::string(name);
    pkt.mPos                      = BlockPos{
        static_cast<int>(pos.x * 8.0f),
        static_cast<int>(pos.y * 8.0f),
        static_cast<int>(pos.z * 8.0f),
    }; // the packet carries positions in 1/8 blocks
    pkt.mVolume                   = volume;
    pkt.mPitch                    = pitch;
    pkt.mLoopCount                = 0;
    pkt.mServerSoundHandle        = std::nullopt;
    pkt.mBypassListenerRangeCheck = false;
    pkt.mPlaybackPositionSeconds  = std::nullopt;
    pkt.sendTo(game::blockPosOf(pos), DimensionType(dimension));
}

void playSound(Actor const& at, std::string_view name, float volume, float pitch) {
    playSound(static_cast<int>(at.getDimensionId()), at.getPosition(), name, volume, pitch);
}

void playAmbient(Actor& actor) { actor.playAmbientSound(); }

void swing(Player& player) {
    player.swing(ActorSwingSource::Interact, HandSlot::Mainhand);
    auto  ptr = makePacket<AnimatePacket>(MinecraftPacketIds::Animate);
    auto& pkt = *ptr;
    pkt.mRuntimeId   = player.getRuntimeID();
    pkt.mAction      = AnimatePacketPayload::Action::Swing;
    pkt.mData        = 0.0f;
    pkt.mSwingSource = ActorSwingSource::Interact;
    pkt.sendTo(static_cast<Actor const&>(player));
}

} // namespace lo::feature::carry
