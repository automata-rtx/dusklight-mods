cmake_minimum_required(VERSION 3.26)
project(dusklight_mods CXX)

# Superproject for our Dusklight graphics mods, built on the official Dusklight mod template
# (https://github.com/TwilitRealm/mod-template): the pinned Dusklight source is fetched by
# DUSKLIGHT_VERSION into ./dusklight (cmake/FetchDusklight.cmake), each mod is packaged as a .dusk
# into ${CMAKE_BINARY_DIR}/mods, and the SDK auto-downloads the per-arch link stub it needs. Only
# the mod sources compile here — the game and its renderer are never built.
#
# The platform is the SCENE NORMAL BUFFER build: automata-rtx/dusklight-ao's
# claude/dusklight-thin-gbuffer-normals-l4l9dc, published as the platform-normals-test prerelease.
# It is upstream Dusklight 008a18c1 (item service + the game-service major-version bump) plus the
# GfxService 1.2 normal-buffer additions, over an aurora carrying the optional normal target and
# the enlarged streaming buffers the shadow mod's cascade replays need.
#
# Two reasons this is the pin rather than upstream:
#
#   1. ABI. Upstream 008a18c1 bumped the GAME SERVICE MAJOR VERSION. Mods built against the older
#      0fc05028 SDK are refused outright by that host — which is why every mod but SMAA (whose
#      imports do not reach it) failed to load rather than merely misbehaving.
#   2. Authored normals. GfxService 1.3 hands mods the game's own interpolated vertex normal
#      (GfxResolveDesc::normal -> GfxResolvedTargets::normal), so Graphics Hub can stop
#      reconstructing every pixel from depth. See docs/authored_normals.md.
#
# THE FORK IS NOW ONE FIELD PAIR WIDE. Everything else the mods use is upstream: GfxService 1.2
# (GfxRenderTargetLayout, get_scene_target_layout, gfx_init_color_target_states, GfxDrawContext
# ::layout) landed upstream in #2305, so the hand-rolled GfxPassTargets vocabulary our fork used to
# carry is gone. What remains fork-local is 1.3's GfxResolveDesc::normal + GfxResolvedTargets
# ::normal. Note GfxDeviceInfo::normal_format NO LONGER EXISTS in any form — the earlier fork's
# offset collision (our normal_format at offset 40 vs upstream's WGPUInstance at the same offset)
# cannot recur because the field is gone rather than relocated. Mod code still goes through
# common/gfx_normal_compat.h, which detects the surviving fields at compile time and degrades to
# "no normal buffer" on a base without them. See docs/normal_buffer_portability.md.
#
# DUSKLIGHT_SDK_STUB_URL must track DUSKLIGHT_VERSION: upstream's version-independent `sdk` release
# does not carry this build's symbols, so the per-arch link stubs come from the same prerelease as
# the game build. (Linux needs no stub at all — game symbols resolve at load via
# --allow-shlib-undefined.) The `platform-normals-test` tag is REPUBLISHED on every push to the
# platform branch, so the URL is stable across re-pins while its assets are not — a stale local
# build/ directory can therefore hold stubs from the previous build. DUSKLIGHT_AURORA_VERSION stays
# unset: the recorded extern/aurora pin (49d644e) is a live branch tip on automata-rtx/aurora-ao.
#
# To move to a newer base game, bump DUSKLIGHT_VERSION and the matching stub URL, and reconfigure.
# Keep the pin matched to the game build actually being run — the game-linked mods resolve hook
# targets by symbol at load, and a mod built against a different base can fail to load outright.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "5ded001c11b0305159cc7e805c21fb2c9d4caf7c")
endif ()

if (NOT DEFINED DUSKLIGHT_REPOSITORY)
    set(DUSKLIGHT_REPOSITORY "https://github.com/automata-rtx/dusklight-ao.git"
            CACHE STRING "Dusklight git repository to fetch from")
endif ()

if (NOT DEFINED DUSKLIGHT_SDK_STUB_URL)
    set(DUSKLIGHT_SDK_STUB_URL
            "https://github.com/automata-rtx/dusklight-ao/releases/download/platform-normals-test"
            CACHE STRING "Base URL for game link stubs downloaded by out-of-tree mod builds")
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# Graphics Hub exports the depth-to-normal service and Celestial Orbit the sun/moon orbit service;
# add both before their consumers (ssilvb, realtime_sun_shadows), which add their include/ for the
# shared service headers.
add_subdirectory(mods/graphics_hub)
add_subdirectory(mods/celestial_orbit)
add_subdirectory(mods/vbao)
add_subdirectory(mods/ssilvb)
add_subdirectory(mods/realtime_sun_shadows)
add_subdirectory(mods/effect_remover)
add_subdirectory(mods/smaa)
