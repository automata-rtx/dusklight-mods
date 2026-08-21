# VBAO — Visibility Bitmask Ambient Occlusion. Service-only (gfx/camera/config/ui/resource/log),
# no game headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
#
# It reads the game's authored surface normals straight from GfxService (get_scene_normals), so it
# has no service imports beyond the stock ones and no dependency on any other mod.
add_mod(vbao
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(vbao PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../deferred_fog/include
    ${CMAKE_SOURCE_DIR}/common)
