add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

set_policy("package.requires_lock", true)

-- LiftOff runs on both the dedicated server (BDS) and the client (local worlds).
-- The levilamina package and the levibuildscript rules read this option.
option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

option("tests")
    set_default(true)
    set_showmenu(true)
    set_description("Build the host-side unit test binary (lo-tests)")
option_end()

add_requires("levilamina 26.51.7", {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0")
if has_config("tests") then
    add_requires("gtest")
end

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

local function lo_common_flags()
    set_languages("c++20")
    set_toolchains("clang-cl")
    add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
    add_cxflags("/utf-8", "/W4")
    add_cxflags(
        "-Wno-microsoft-cast",
        "-Wno-invalid-offsetof",
        "-Wno-c++2b-extensions",
        "-Wno-microsoft-include",
        "-Wno-overloaded-virtual",
        "-Wno-ignored-qualifiers",
        "-Wno-missing-field-initializers",
        "-Wno-potentially-evaluated-expression",
        "-Wno-pragma-system-header-outside-header",
        {tools = {"clang_cl"}}
    )
end

-- Pure, game-independent logic (src/core). No mc/ll headers allowed here.
target("lo-core")
    set_kind("static")
    lo_common_flags()
    set_exceptions("none")
    add_cxflags("/EHa")
    add_packages("nlohmann_json", {public = true})
    add_includedirs("src", {public = true})
    add_headerfiles("src/core/**.h")
    add_files("src/core/**.cpp")

-- The mod itself. Every translation unit is linked directly into the DLL (not through a static
-- library) so that static self-registration objects (LO_REGISTER_*) are never stripped.
target("LiftOff")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    set_kind("shared")
    lo_common_flags()
    set_exceptions("none") -- To avoid conflicts with /EHa.
    add_cxflags("/EHa", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
    add_cxflags("/EHs", {tools = {"clang_cl"}})
    set_symbols("debug")
    add_deps("lo-core")
    add_packages("levilamina", "nlohmann_json")
    add_includedirs("src")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp|core/**.cpp")
    if is_config("target_type", "client") then
        add_defines("LO_CLIENT=1")
    else
        add_defines("LO_SERVER=1")
    end
    -- Assemble bin/<target_type>/LiftOff/{dll,pdb,manifest.json,lang/,packs/}.
    after_build(function (target)
        import("core.project.config")
        import("assemble", {rootdir = path.join(os.projectdir(), "tools", "xmake")})
        assemble(target, config.get("target_type") or "server")
    end)

if has_config("tests") then
    target("lo-tests")
        set_kind("binary")
        set_default(false)
        lo_common_flags()
        set_exceptions("cxx")
        add_deps("lo-core")
        add_packages("gtest", "nlohmann_json")
        add_files("tests/**.cpp")
        add_tests("default")
end
