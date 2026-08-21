# SMAA — subpixel morphological antialiasing (SMAA 1x). Service-only (gfx/config/ui/resource/log),
# no game headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
#
# Its geometric edge detector reads the game's authored surface normals straight from GfxService
# (get_scene_normals), so it depends on no other mod.
add_mod(smaa
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(smaa PRIVATE ${CMAKE_SOURCE_DIR}/common)
