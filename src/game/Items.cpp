#include "game/Items.h"

#include "mc/world/item/ItemStack.h"

namespace lo::game {

ItemStack makeItem(std::string_view id, int count, int aux) {
    ItemStack stack;
    stack.reinit(id, count, aux);
    return stack;
}

} // namespace lo::game
