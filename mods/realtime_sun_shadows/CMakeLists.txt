# Realtime Sun Shadows — game-linked (replays the game draw list into light-space shadow maps and
# hooks game functions). The mod SDK is configured once by the top-level CMakeLists.
add_mod(realtime_sun_shadows
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Hard dependency on the Depth to Normal provider mod for receiver normals (shared service header).
target_include_directories(realtime_sun_shadows PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../depth_to_normal/include)
