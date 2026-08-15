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
#   2. Authored normals. GfxService 1.2 hands mods the game's own interpolated vertex normal
#      (GfxResolveDesc::normal -> GfxResolvedTargets::normal), so Graphics Hub can stop
#      reconstructing every pixel from depth. See docs/authored_normals.md.
#
# The earlier fork attempt failed on an OFFSET COLLISION, and this pin does not repeat it: back
# then our fork appended GfxDeviceInfo::normal_format at offset 40 and upstream independently
# appended WGPUInstance there, so a fork-built mod read a live pointer as a texture format. Here
# normal_format is appended AFTER upstream's own instance/adapter fields, on top of upstream rather
# than beside it, so there is no slot being claimed twice. Mod code still goes through
# common/gfx_normal_compat.h, which detects the fields at compile time and degrades to "no normal
# buffer" on a base without them. See docs/normal_buffer_portability.md.
#
# DUSKLIGHT_SDK_STUB_URL must track DUSKLIGHT_VERSION: upstream's version-independent `sdk` release
# does not carry this build's symbols, so the per-arch link stubs come from the same prerelease as
# the game build. (Linux needs no stub at all — game symbols resolve at load via
# --allow-shlib-undefined.) DUSKLIGHT_AURORA_VERSION stays unset: the recorded extern/aurora pin
# (f8d8c035) is a live branch tip on automata-rtx/aurora-ao and resolves on its own.
#
# To move to a newer base game, bump DUSKLIGHT_VERSION and the matching stub URL, and reconfigure.
# Keep the pin matched to the game build actually being run — the game-linked mods resolve hook
# targets by symbol at load, and a mod built against a different base can fail to load outright.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "629708397390998b6f30357123a5957f45e79359")
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
