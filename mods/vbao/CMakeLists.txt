# VBAO — Visibility Bitmask Ambient Occlusion. Service-only (gfx/camera/config/ui/resource/log),
# no game headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
#
# It reads the game's authored surface normals from the same resolve that gives it depth
# (GfxResolveDesc::normal -> GfxResolvedTargets::normal, via common/gfx_normal_compat.h), so it has
# no service imports beyond the stock ones and no dependency on any other mod.
add_mod(vbao
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(vbao PRIVATE ${CMAKE_SOURCE_DIR}/common)
