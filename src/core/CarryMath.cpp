#include "core/CarryMath.h"

#include <algorithm>

namespace lo::core::carry {

namespace {
int finish(int i, double multiplier) {
    i = static_cast<int>(static_cast<double>(i) * multiplier);
    return std::clamp(i, 0, kMaxSlowness);
}
} // namespace

int blockSlowness(std::size_t nbtSize, bool heavyTiles, double multiplier) {
    int i = heavyTiles ? static_cast<int>(std::min<std::size_t>(nbtSize / 500, 1u << 20)) : 1;
    return finish(i, multiplier);
}

int entitySlowness(float height, float width, float scaleFactor, bool heavyEntities, double multiplier) {
    int i = heavyEntities ? static_cast<int>(height * width * (scaleFactor * scaleFactor)) : 1;
    return finish(i, multiplier);
}

double reachScale(bool scaleDistance, bool creative, bool entity) {
    if (!scaleDistance || !creative) return 1.0;
    return entity ? (3.0 + 2.0) / 3.0 : (4.5 + 0.5) / 4.5;
}

bool withinReach(double distSq, double maxDistance, double scale) {
    return distSq < maxDistance * maxDistance * scale * scale;
}

} // namespace lo::core::carry
