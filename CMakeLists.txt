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
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "9361fbd9ea6f14a4cc0307c9ca9252e6f6ffb73d")
endif ()

# Knob 1: fetch the SDK from our fork instead of upstream Dusklight (enlarged buffers + the exact
# game build the user runs, published as the platform-v2-test release).
if (NOT DEFINED DUSKLIGHT_REPOSITORY)
    set(DUSKLIGHT_REPOSITORY "https://github.com/automata-rtx/dusklight-ao.git"
            CACHE STRING "Dusklight git repository to fetch from")
endif ()

# Knob 2: the SDK auto-downloads the per-arch link stub (Windows/macOS/Android) from this URL.
# Upstream's default (encounter/dusklight) is private, so point it at our public platform-v2-test
# release, which publishes windows-<arch>.lib and stub-<platform> as top-level assets. (Linux needs
# no stub — game symbols resolve at load via --allow-shlib-undefined.)
if (NOT DEFINED DUSKLIGHT_SDK_STUB_URL)
    set(DUSKLIGHT_SDK_STUB_URL
            "https://github.com/automata-rtx/dusklight-ao/releases/download/platform-v2-test"
            CACHE STRING "Base URL for game link stubs downloaded by out-of-tree mod builds")
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# Graphics Hub exports the depth-to-normal service; add it before its consumers (ssilvb,
# realtime_sun_shadows), which add its include/ for the shared service header.
add_subdirectory(mods/graphics_hub)
add_subdirectory(mods/vbao)
add_subdirectory(mods/ssilvb)
add_subdirectory(mods/realtime_sun_shadows)
add_subdirectory(mods/effect_remover)
add_subdirectory(mods/smaa)
