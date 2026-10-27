#pragma once

#include <concepts>
#include <string_view>

#include "mod/Registry.h"

namespace lo {

/// A unit of the mod that hooks into the game. Any class with `install()` and `uninstall()`
/// can be a module; register it with `LO_REGISTER_MODULE(T)` in its .cpp file.
///
/// - `install()` runs on mod enable, `uninstall()` on disable (reverse order). Both run on the
///   server thread. Undo everything in `uninstall()` (listeners, hooks, commands' state).
/// - If `T` has `static T& get()`, that instance is used; otherwise the registrar owns one.
/// - Optional `load()` runs at mod load (before the world is created; BDS: before the server
///   starts). Use it only for things that must happen that early (e.g. world pack config).
/// - Order: lower `order` installs first. Foundation modules use negative orders
///   (see `ModuleOrder`); feature units should use the default 0.
template <class T>
concept ModuleType = requires(T& t) {
    t.install();
    t.uninstall();
};

namespace ModuleOrder {
inline constexpr int Config   = -1000; ///< configs, translations
inline constexpr int Storage  = -900;  ///< world DB
inline constexpr int Items    = -800;  ///< pack deployer
inline constexpr int Net      = -700;  ///< client/server protocol (when a unit adds one)
inline constexpr int Services = -600;  ///< service seams (when a unit adds them)
inline constexpr int Commands = -500;  ///< /carryon command tree
inline constexpr int Feature  = 0;     ///< default for feature units
} // namespace ModuleOrder

struct ModuleEntry {
    std::string_view name;
    int              order;
    void (*install)();
    void (*uninstall)();
    void (*load)(); ///< optional (nullptr): `T::load()` runs at mod load, before the world exists
};

using ModuleRegistry = StaticRegistry<ModuleEntry>;

namespace detail {
template <ModuleType T>
T& moduleInstance() {
    if constexpr (requires { { T::get() } -> std::same_as<T&>; }) {
        return T::get();
    } else {
        static T instance;
        return instance;
    }
}
template <ModuleType T>
struct ModuleRegistrar {
    ModuleRegistrar(std::string_view name, int order) {
        void (*loadFn)() = nullptr;
        if constexpr (requires(T& t) { t.load(); }) loadFn = [] { moduleInstance<T>().load(); };
        ModuleRegistry::add(ModuleEntry{
            name,
            order,
            [] { moduleInstance<T>().install(); },
            [] { moduleInstance<T>().uninstall(); },
            loadFn,
        });
    }
};
} // namespace detail

/// Runs the optional `load()` of all registered modules (sorted by order). Mod entry only.
void loadModules();
/// Installs all registered modules (sorted by order). Called by the mod entry only.
void installModules();
/// Uninstalls all installed modules in reverse order. Called by the mod entry only.
void uninstallModules();

} // namespace lo

/// Registers module `T` (default order 0). Put it in the module's .cpp file.
#define LO_REGISTER_MODULE(T)                                                                                         \
    static ::lo::detail::ModuleRegistrar<T> LO_UNIQUE(lo_module_registrar_)(#T, ::lo::ModuleOrder::Feature)
/// Registers module `T` with an explicit install order.
#define LO_REGISTER_MODULE_ORDERED(T, order)                                                                          \
    static ::lo::detail::ModuleRegistrar<T> LO_UNIQUE(lo_module_registrar_)(#T, (order))
