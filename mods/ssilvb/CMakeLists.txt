# SSILVB — Screen Space Indirect Lighting with Visibility Bitmask (Therrien et al. 2023).
# Service-only (gfx/camera/config/ui/resource/log), no game headers or hooks. The mod SDK is
# configured once by the top-level CMakeLists.
add_mod(ssilvb
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Hard dependency on the depth-to-normal service (shared header), now exported by Graphics Hub:
# the GI accumulate needs a surface normal at every marched sample.
target_include_directories(ssilvb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../graphics_hub/include)
