# Graphics Hub — combined host for screen-space infrastructure (Depth to Normal provider service +
# Deferred Fog). Game-linked (Deferred Fog hooks game functions) + webgpu. The mod SDK is
# configured once by the top-level CMakeLists.
add_mod(graphics_hub
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# The shared world-normal service header (depth_to_normal_service.h) lives in include/ so consumer
# mods (ssilvb, realtime_sun_shadows) add this directory to their include path and import the
# "dev.automata.depth_to_normal" service — which Graphics Hub now exports.
target_include_directories(graphics_hub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
