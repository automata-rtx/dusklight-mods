# VBAO — Visibility Bitmask Ambient Occlusion. Service-only (gfx/camera/config/ui/resource/log),
# no game headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
#
# It reads the game's authored surface normals from the same resolve that gives it depth
# (GfxResolveDesc::normal -> GfxResolvedTargets::normal, via common/gfx_normal_compat.h), so it
# needs nothing from the game itself.
#
# It DOES require Deferred Fog, which is why deferred_fog/include is on the include path: mod.cpp
# takes a required import of DeferredFogService so the loader will not activate VBAO without it.
# That is a header-only dependency - nothing links against deferred_fog and the build order between
# the two targets does not matter - so this stays a plain include directory rather than a
# target_link_libraries.
add_mod(vbao
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(vbao PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/mods/deferred_fog/include)
