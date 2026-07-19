# VBAO — Visibility Bitmask Ambient Occlusion. Service-only (gfx/camera/config/ui/resource/log),
# no game headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
add_mod(vbao
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Optional integration with the depth-to-normal service (shared header), now exported by
# Graphics Hub.
target_include_directories(vbao PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../graphics_hub/include)
