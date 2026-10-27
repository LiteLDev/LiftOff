#pragma once

#include <string>

namespace lo::core {

/// Block position plus dimension id (0 overworld, 1 nether, 2 end, others as the game reports).
struct GlobalPos {
    int dim = 0;
    int x = 0, y = 0, z = 0;

    friend bool operator==(GlobalPos const&, GlobalPos const&) = default;
    friend auto operator<=>(GlobalPos const&, GlobalPos const&) = default;
    [[nodiscard]] std::string key() const {
        return std::to_string(dim) + ":" + std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
    }
};

} // namespace lo::core
