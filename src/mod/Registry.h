#pragma once

#include <vector>

/// Token pasting helpers for unique static registration objects.
#define LO_CONCAT_IMPL(a, b) a##b
#define LO_CONCAT(a, b)      LO_CONCAT_IMPL(a, b)
#define LO_UNIQUE(prefix)    LO_CONCAT(prefix, __COUNTER__)

namespace lo {

/// A process-wide list of `Entry`, filled by static registrar objects before `main`/mod load.
/// The function-local static avoids static-initialization-order problems.
///
/// Registrations live in translation units linked directly into the mod DLL (never into a
/// static library), so the linker can't drop them.
template <class Entry>
class StaticRegistry {
public:
    static std::vector<Entry>& entries() {
        static std::vector<Entry> v;
        return v;
    }
    static void add(Entry e) { entries().push_back(std::move(e)); }
};

} // namespace lo
