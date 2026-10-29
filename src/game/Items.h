#pragma once

// Item helpers (server thread).

#include <string_view>

class ItemStack;

namespace lo::game {

/// An item stack of `id` ("minecraft:stick", "liftoff:..."): `count` items with aux value (damage /
/// legacy data) `aux`. Returns a null stack (`isNull()`) when the id is unknown. `ItemStack` has no
/// (name, count, aux) constructor, so use this.
[[nodiscard]] ItemStack makeItem(std::string_view id, int count = 1, int aux = 0);

} // namespace lo::game
