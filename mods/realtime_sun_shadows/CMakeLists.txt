# Realtime Sun Shadows — game-linked (replays the game draw list into light-space shadow maps and
# hooks game functions). The mod SDK is configured once by the top-level CMakeLists.
add_mod(realtime_sun_shadows
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Depends on the depth-to-normal service for receiver normals (shared header), now exported by
# Graphics Hub.
target_include_directories(realtime_sun_shadows PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../graphics_hub/include)
