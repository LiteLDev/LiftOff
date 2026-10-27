#pragma once

#include "ll/api/mod/NativeMod.h"

namespace lo {

/// Mod entry point. It only drives the module registry (`LO_REGISTER_MODULE`); features never
/// need to edit this file.
class Mod {
public:
    static Mod& getInstance();

    Mod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    [[nodiscard]] bool isEnabled() const { return mEnabled; }

private:
    ll::mod::NativeMod& mSelf;
    bool                mEnabled = false;
};

} // namespace lo
