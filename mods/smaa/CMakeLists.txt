# SMAA — subpixel morphological antialiasing. Service-only (gfx/config/ui/resource/log), no game
# headers or hooks. The mod SDK is configured once by the top-level CMakeLists.
add_mod(smaa
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Optional integration with the depth-to-normal service (shared header), exported by Graphics Hub:
# the normal + depth drive geometric edge detection. Optional so the mod still loads and does
# luma-only SMAA when the provider is absent.
target_include_directories(smaa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../graphics_hub/include)
