# Lantern Position — moves the lit lantern's placement on Link's belt (game-linked: pre/post-hooks
# daAlink_c::setItemMatrix and rebuilds mpKanteraModel's base matrix). No shaders, no webgpu. The
# mod SDK is configured once by the top-level CMakeLists.
add_mod(lantern_position
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
