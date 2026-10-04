cmake_minimum_required(VERSION 3.26)

# Build type: RelWithDebInfo, set before project().
#
# On Windows a Release link (/OPT:REF) discards the DEFINE_HOOK records the game's loader scans for,
# so a game-linked mod loads and silently does nothing; RelWithDebInfo keeps them. Setting the
# default before project() matters for single-config generators with MSVC (Ninja, NMake), where
# project() would otherwise cache Debug and the SDK's own RelWithDebInfo fallback would never apply.
# Multi-config generators (Visual Studio, Ninja Multi-Config) ignore CMAKE_BUILD_TYPE: build those
# with --config RelWithDebInfo. CI configures Ninja with -DCMAKE_BUILD_TYPE=RelWithDebInfo.
if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(dusklight_mods CXX)

# Graphics mods for Dusklight, laid out as a monorepo on the official mod template
# (https://github.com/TwilitRealm/mod-template). cmake/FetchDusklight.cmake fetches the pinned
# Dusklight source (game headers, mod SDK, and the aurora renderer for reference) into ./dusklight;
# each mod is packaged as a .dusk into ${CMAKE_BINARY_DIR}/mods. Only the mod sources compile here.
#
# The pin. DUSKLIGHT_VERSION is upstream TwilitRealm/dusklight's v2.0.0 release tag (GameService 2.0,
# GfxService 1.3). A tag rather than a SHA, because it names a build users can install, and the game
# build must match the pin: game-linked mods resolve their hook targets by symbol name at load.
#
# To move to a newer base game, bump DUSKLIGHT_VERSION and follow CONTRIBUTING.md "Moving to a newer
# game build": read the new SDK headers, re-check every hook target, fix drifted source citations
# (tools/check_source_citations.py), and test in-game. A clean build proves none of that.
#
# Authored normals come from GfxService 1.3 (GfxResolveDesc::normal -> GfxResolvedTargets::normal),
# reached through common/gfx_normal_compat.h. The snapshot latches (the first request returns null
# and enables it for the next frame), and the scene pass gains a normal attachment when it does, so
# scene-pass pipelines are built lazily and rebuilt on GfxDrawContext::layout.key
# (common/gfx_scene_pass.h). See docs/normal_buffer_portability.md.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.0")
endif ()

if (NOT DEFINED DUSKLIGHT_REPOSITORY)
    set(DUSKLIGHT_REPOSITORY "https://github.com/TwilitRealm/dusklight.git"
            CACHE STRING "Dusklight git repository to fetch from")
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# Which mods build. CI packages whatever lands in build/mods, so this list is the release set.
#
# This branch (claude/reshade-bridge-*) builds only the ReShade Bridge: the Dusklight mod
# (reshade_bridge.dusk, game-linked) and, on Windows x64, its ReShade add-on
# (dusklight_reshade_bridge.addon64), both from mods/reshade-bridge. The other mods' sources are
# main's, untouched and not built here; build them from main.
add_subdirectory(mods/reshade-bridge)

# reshade-bridge is game-linked: after any pin bump, re-check its DEFINE_HOOK target
# (dComIfGd_drawXluListDark) by name in the new tree and test it in-game; a clean compile does not
# prove the hook resolves.
