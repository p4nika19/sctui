set_project("sctui")
set_version("1.0.0")
set_languages("c++23")

add_rules("mode.release", "mode.debug")

-- Enforce Clang toolchain
set_toolchains("clang")

-- Manage dependencies via xrepo
add_requires("ftxui v7.0.3", {configs = {cxflags = "-fPIC"}})
add_requires("simdjson v4.6.11")
add_requires("miniaudio 0.11.25")
add_requires("nlohmann_json v3.12.0")
add_requires("stb 2026.03.18")

target("sctui")
    set_kind("binary")
    add_files("src/*.cpp")
    add_files("src/models/*.cpp")
    add_files("src/core/*.cpp")
    add_files("src/net/*.cpp")
    add_files("src/audio/*.cpp")
    add_files("src/ui/*.cpp")
    add_files("src/ui/views/*.cpp")
    add_files("src/mpris/*.cpp")
    add_files("src/discord/*.cpp")

    add_includedirs("include")

    add_packages("ftxui", "simdjson", "miniaudio", "nlohmann_json", "stb")
    add_syslinks("curl", "pthread", "m", "dl", "asound", "pulse", "systemd", "sqlite3", "crypto")

    -- Enforce mold linker
    add_ldflags("-fuse-ld=mold", {force = true})

    -- Strict Diagnostics
    add_cxflags("-Wall", "-Wextra", "-Wpedantic", "-Wshadow", {force = false})
    add_defines("_FORTIFY_SOURCE=2")

    if is_mode("release") then
        set_optimize("fastest")
        add_cxflags(
            "-O3",
            "-flto",
            "-march=native",
            "-mtune=native",
            "-fno-math-errno",
            "-fno-trapping-math",
            "-fomit-frame-pointer",
            "-fvectorize",
            "-funroll-loops",
            {force = true}
        )
        add_ldflags("-flto", {force = true})
    elseif is_mode("debug") then
        set_symbols("debug")
        set_optimize("none")
        add_cxflags("-g3", {force = true})
    end

target("test_suite")
    set_kind("binary")
    add_files("tests/test_suite.cpp")
    add_files("src/models/track.cpp")
    add_files("src/core/config.cpp")
    add_files("src/core/logger.cpp")
    add_files("src/core/cover_art.cpp")
    add_files("src/core/cookie_extractor.cpp")
    add_files("src/core/track_cache.cpp")
    add_files("src/core/thread_pool.cpp")
    add_files("src/net/http_client.cpp")
    add_files("src/net/soundcloud_api.cpp")
    add_files("src/mpris/mpris_server.cpp")
    add_files("src/discord/discord_rpc.cpp")
    add_files("src/audio/drm_daemon.cpp")
    add_files("src/ui/views/settings_view.cpp")

    add_includedirs("include")
    add_packages("ftxui", "simdjson", "nlohmann_json", "stb")
    add_syslinks("curl", "pthread", "m", "systemd", "sqlite3", "crypto")
    add_ldflags("-fuse-ld=mold", {force = true})
