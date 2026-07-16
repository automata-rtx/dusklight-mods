# Depth to Normal — reconstructs a world-space surface normal from the depth buffer and publishes
# it as a mod-exported service. Service-only. The mod SDK is configured by the top-level CMakeLists.
add_mod(depth_to_normal
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# The shared service header (depth_to_normal_service.h) lives in include/ so consumer mods can
# add the same directory to their include path and import the service.
target_include_directories(depth_to_normal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
