# Celestial Orbit — retilts the sun/moon travel path (game-linked: post-hooks
# dScnKy_env_light_c::setSunpos and rewrites sun_pos / moon_pos). No shaders, no webgpu. The mod
# SDK is configured once by the top-level CMakeLists.
add_mod(celestial_orbit
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)

# The shared orbit service header (celestial_orbit_service.h) lives in include/ so consumer mods
# (realtime_sun_shadows) add this directory to their include path and import the
# "dev.automata.celestial_orbit" service — which carries the shared transform both sides apply.
target_include_directories(celestial_orbit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
