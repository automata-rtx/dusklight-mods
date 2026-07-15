cmake_minimum_required(VERSION 3.26)
project(dusklight_mods CXX)

# Superproject for our Dusklight graphics mods, built on the official Dusklight mod template
# (https://github.com/TwilitRealm/mod-template). Instead of vendoring the game/SDK as a submodule,
# the pinned Dusklight source is fetched by DUSKLIGHT_VERSION into ./dusklight (see
# cmake/FetchDusklight.cmake), and each mod is packaged as a .dusk into ${CMAKE_BINARY_DIR}/mods.
# Only the mod sources compile here — the game and its renderer are never built.
#
# The Dusklight version to build against: a release tag or commit SHA. To move the mods to a newer
# base game, change this one line and reconfigure.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "c18dc4223f45c7ef3a1b656987f58f2352f6c7ac")
endif ()

# We build against OUR platform fork (automata-rtx/dusklight-ao) rather than upstream Dusklight:
# it carries the enlarged aurora streaming buffers the shadow mod needs, and the pinned commit
# above matches the game build the user runs (published as the platform-v2-test release). Pass
# -DDUSKLIGHT_REPOSITORY=... or -DDUSKLIGHT_DIR=<checkout> to override.
if (NOT DEFINED DUSKLIGHT_REPOSITORY)
    set(DUSKLIGHT_REPOSITORY "https://github.com/automata-rtx/dusklight-ao.git"
            CACHE STRING "Dusklight git repository to fetch from")
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# Provider mod first, then its consumers (they add its include/ for the shared service header).
add_subdirectory(mods/depth_to_normal)
add_subdirectory(mods/vbao)
add_subdirectory(mods/realtime_sun_shadows)
add_subdirectory(mods/deferred_fog)

# On Windows/macOS/Android a game-linked mod (FEATURES game|webgpu) needs DUSK_GAME_EXE — the
# per-arch import library (Windows) or link stub (macOS bundle_loader / Android). Our pinned
# platform predates the SDK's auto-download of these, so CI provides them from the platform-v2-test
# release (see .github/workflows/build.yml); Linux links with --allow-shlib-undefined and needs
# nothing. Windows MUST use clang-cl: plain cl strips the SDK's modmeta/DEFINE_HOOK records and
# hooks never register (guarded by tools/hook_repro). Locally, pass
# -DDUSK_GAME_EXE=<sdk/windows-<arch>.lib | sdk/stub-...> when building a non-Linux target.

# Offline WGSL validator (Dawn Null backend). Used by CI and local shader iteration:
#   cmake -B build -DMODS_BUILD_TOOLS=ON && cmake --build build --target wgsl_validate
#   build/wgsl_validate mods/vbao/res/vbao.wgsl
option(MODS_BUILD_TOOLS "Build repo tools (WGSL validator)" OFF)
if (MODS_BUILD_TOOLS)
    add_executable(wgsl_validate tools/wgsl_validate.cpp)
    target_compile_features(wgsl_validate PRIVATE cxx_std_20)
    target_link_libraries(wgsl_validate PRIVATE dawn::webgpu_dawn)
    if (TARGET dawn::dawncpp_headers)
        target_link_libraries(wgsl_validate PRIVATE dawn::dawncpp_headers)
    endif ()
endif ()
