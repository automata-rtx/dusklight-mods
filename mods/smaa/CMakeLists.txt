# SMAA: subpixel morphological antialiasing (SMAA 1x). Service-only (gfx/config/ui/resource/log):
# no game headers, no hooks. The mod SDK is configured once by the top-level CMakeLists.
#
# The geometric edge detector reads the game's authored normals straight from GfxService
# (GfxResolveDesc::normal -> GfxResolvedTargets::normal, through common/gfx_normal_compat.h), so the
# mod depends on no other mod. common/ also provides gfx_scene_pass.h.
add_mod(smaa
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(smaa PRIVATE ${CMAKE_SOURCE_DIR}/common)
