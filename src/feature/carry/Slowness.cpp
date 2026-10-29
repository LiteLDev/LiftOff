#include "feature/carry/Slowness.h"

#include "mc/world/phys/AABB.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"

#include "core/CarryMath.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Checks.h"
#include "mod/Config.h"

namespace lo::feature::carry {

float entityHeight(Actor const& entity) {
    auto const& box = entity.getAABB();
    return box.max.y - box.min.y;
}
float entityWidth(Actor const& entity) {
    auto const& box = entity.getAABB();
    return box.max.x - box.min.x;
}

float playerScale(Player const& player) {
    float s = player.mScale;
    return s > 0.0f ? s : 1.0f;
}

int blockAmplifier(CarryData const& data) {
    auto const& s = config::get<CarryConfig>().settings;
    return core::carry::blockSlowness(data.nbtSize(), s.heavyTiles, s.blockSlownessMultiplier);
}

int entityAmplifier(Actor const& entity, Player const& carrier) {
    auto const& s      = config::get<CarryConfig>().settings;
    float       factor = s.relativePlayerScale ? 1.0f / playerScale(carrier) : 1.0f;
    return core::carry::entitySlowness(
        entityHeight(entity),
        entityWidth(entity),
        factor,
        s.heavyEntities,
        s.entitySlownessMultiplier
    );
}

int playerAmplifier() { return core::carry::kPlayerSlowness; }

void applySlowness(Player& player, int amplifier) {
    if (isCreative(player) && !config::get<CarryConfig>().settings.slownessInCreative) return;
    MobEffectInstance effect(kSlownessEffectId, EffectDuration{kSlownessDuration});
    effect.mAmplifier     = amplifier;
    effect.mAmbient       = false;
    effect.mEffectVisible = false;
    player.addEffect(effect);
}

void removeSlowness(Player& player) {
    if (player.getEffect(kSlownessEffectId)) player.removeEffect(kSlownessEffectId);
}

int slownessAmplifier(Player const& player) {
    auto const* e = player.getEffect(kSlownessEffectId);
    return e ? e->mAmplifier : -1;
}

} // namespace lo::feature::carry
