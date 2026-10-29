#pragma once

// U11: native third-person rendering of carried objects (docs/SPEC.md §4.3), client build only.
//
// Every player the client draws (other players, and the local player in the F5 views) that carries a
// block or an entity according to the client mirror (`lo::client::carry::carriedCopy`) gets its carry drawn
// after the player model: a block as its item, an entity as a client-only display actor built from the
// carried NBT. The transform is Carry On's (CarryTransform.h) in the carrier's interpolated frame.
//
// Caches (per carrier unique id): the visual, the item stack and the display actor. They are rebuilt when
// the carry changes, dropped when the carry ends, when the carrier has not been drawn for a while (out of
// view) and when the client leaves the level.
//
// The functions below are for selftests (any thread).

#ifdef LO_CLIENT

#include <array>
#include <cstdint>

namespace lo::client::thirdperson {

struct DrawStats {
    std::uint64_t frames = 0;    ///< frames the carry of this carrier was drawn in
    int           kind   = 0;    ///< `CarryVisual::Kind` of the last draw
    bool          displayActor = false; ///< an entity was drawn through a display actor
    std::array<float, 3> center{0.f, 0.f, 0.f}; ///< world position of the drawn object's centre (last draw)
};

/// Draw statistics of the carrier `uniqueId` since the last `resetStats` (frames 0 = never drawn).
[[nodiscard]] DrawStats statsOf(std::int64_t uniqueId);
void                    resetStats();
/// The renderer holds a cache entry (visual / display actor) for the carrier.
[[nodiscard]] bool hasCacheEntry(std::int64_t uniqueId);

/// The client's camera perspective: 0 first person, 1 third person back, 2 third person front
/// (-1 outside a level). `setPerspective` runs on the client thread.
[[nodiscard]] int perspective();
void              setPerspective(int value);

} // namespace lo::client::thirdperson

#endif // LO_CLIENT
