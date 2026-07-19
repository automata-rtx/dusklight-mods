# Projected Shadow Removal — game-linked (suppresses TP's "moya" projected-ground-shadow draw).
# The mod SDK is configured once by the top-level CMakeLists.
add_mod(projected_shadow_removal
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
