cmake_minimum_required(VERSION 3.26)
project(dusklight_mods CXX)

# Superproject for our Dusklight graphics mods, built on the official Dusklight mod template
# (https://github.com/TwilitRealm/mod-template): the pinned Dusklight source is fetched by
# DUSKLIGHT_VERSION into ./dusklight (cmake/FetchDusklight.cmake), each mod is packaged as a .dusk
# into ${CMAKE_BINARY_DIR}/mods, and the SDK auto-downloads the per-arch link stub it needs. Only
# the mod sources compile here — the game and its renderer are never built.
#
# Two cache knobs point the stock template at our platform fork (which carries the enlarged aurora
# streaming buffers the shadow mod needs); everything else is the template unchanged.
#
# To move to a newer base game, bump DUSKLIGHT_VERSION (one line) and reconfigure.
#
# Currently the THIN G-BUFFER platform (tag platform-gbuffer-test, branch
# claude/thin-gbuffer-authored-normals-wgqupt). It appends fields to four gfx service structs
# (GfxDeviceInfo::normal_format, GfxDrawContext::normal_format, GfxResolveDesc::normal,
# GfxResolvedTargets::normal[_format]). The host validates callers with
# `caller->struct_size < sizeof(host struct) -> reject`, so mods built against the OLDER SDK are
# rejected outright by this game build: every webgpu mod died with "failed to query device info"
# and the service consumers then failed on the missing depth_to_normal import. Building against
# this SDK fixes that, and stays compatible with platform-v2-test as well - a LARGER struct_size
# passes the older host's check and the appended fields simply keep their GFX_*_INIT defaults
# (normal buffer absent), which is exactly the right semantic there.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "b96bf5ec01c09d93c0957bae72f713081ca58f2f")
endif ()

# The thin-gbuffer commit records an extern/aurora pin (5aa41bf5) that no longer exists: the
# aurora-ao branch was force-pushed to 3ba95790 ("docs: sync thin g-buffer doc") after the game
# build was cut, leaving the submodule pointer dangling, so `git submodule update` fails with
# "upload-pack: not our ref". Aurora contributes only the Dawn/WebGPU headers to a MOD build (no
# aurora source is compiled into a .dusk), and 3ba95790 is the same code with an amended doc
# commit, so overriding the pin is safe here. Remove this once dusklight-ao re-points the
# submodule at a commit that exists.
if (NOT DEFINED DUSKLIGHT_AURORA_VERSION)
    set(DUSKLIGHT_AURORA_VERSION "3ba9579091e4011bdb4797b4c6c3d08085651416"
            CACHE STRING "Override for the extern/aurora submodule commit (empty = use the pin)")
endif ()

# Knob 1: fetch the SDK from our fork instead of upstream Dusklight (enlarged buffers + the exact
# game build the user runs, published as the platform-v2-test release).
if (NOT DEFINED DUSKLIGHT_REPOSITORY)
    set(DUSKLIGHT_REPOSITORY "https://github.com/automata-rtx/dusklight-ao.git"
            CACHE STRING "Dusklight git repository to fetch from")
endif ()

# Knob 2: the SDK auto-downloads the per-arch link stub (Windows/macOS/Android) from this URL.
# Upstream's default (encounter/dusklight) is private, so point it at our public release, which
# publishes windows-<arch>.lib and stub-<platform> as top-level assets. Must match
# DUSKLIGHT_VERSION above: platform-gbuffer-test for the thin g-buffer platform, platform-v2-test
# for the stable one. (Linux needs no stub — game symbols resolve at load via
# --allow-shlib-undefined.)
if (NOT DEFINED DUSKLIGHT_SDK_STUB_URL)
    set(DUSKLIGHT_SDK_STUB_URL
            "https://github.com/automata-rtx/dusklight-ao/releases/download/platform-gbuffer-test"
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
