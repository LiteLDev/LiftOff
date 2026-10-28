#include "feature/carry/CarryConfig.h"

#include <algorithm>

#include "mod/Config.h"

LO_REGISTER_CONFIG("carryon", lo::feature::carry::CarryConfig);
LO_REGISTER_CONFIG("carryon_client", lo::feature::carry::CarryClientConfig);

namespace lo::feature::carry {

void clampConfig(CarryConfig& cfg) {
    auto& s                    = cfg.settings;
    s.maxDistance              = std::max(s.maxDistance, 0.0);
    s.maxEntityWidth           = std::clamp(s.maxEntityWidth, 0.0, 10.0);
    s.maxEntityHeight          = std::clamp(s.maxEntityHeight, 0.0, 10.0);
    s.blockSlownessMultiplier  = std::max(s.blockSlownessMultiplier, 0.0);
    s.entitySlownessMultiplier = std::max(s.entitySlownessMultiplier, 0.0);
    s.maxEntityStackLimit      = std::max(s.maxEntityStackLimit, 1);
}

} // namespace lo::feature::carry
