# Effect Remover — combined game-linked mod that removes built-in TP fake-shading features
# (moya projected shadows + terrain shadow overlay + baked vertex lighting). The mod SDK is
# configured once by the top-level CMakeLists.
add_mod(effect_remover
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
