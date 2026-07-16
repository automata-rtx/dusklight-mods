# Deferred Fog — game-linked (suppresses per-draw fog and re-applies it as a fullscreen pass,
# hooking game functions). The mod SDK is configured once by the top-level CMakeLists.
add_mod(deferred_fog
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)
