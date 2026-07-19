# Terrain Shadow Removal — game-linked (skips the room terrain's BTK-animated shadow-overlay
# materials). The mod SDK is configured once by the top-level CMakeLists.
add_mod(terrain_shadow_removal
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
