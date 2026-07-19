# Unbaked Vertex Lighting — game-linked (hooks the J3D model loader to flatten the vertex
# colors that carry TP's baked lighting). The mod SDK is configured once by the top-level
# CMakeLists.
add_mod(vertex_unbake
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
