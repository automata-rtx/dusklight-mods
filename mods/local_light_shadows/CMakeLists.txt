# Local Light Shadows - game-linked (reads the game's point-light registry and replays the game
# draw list into a light-space shadow map from the nearest local light). The mod SDK is configured
# once by the top-level CMakeLists. Standalone: no service dependencies (the receiver normal is
# reconstructed inline in the composite shader).
add_mod(local_light_shadows
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)
