# Deferred Fog — re-applies the game's fog after screen-space effects. Game-linked (it hooks the
# game's fog setup) + webgpu. The mod SDK is configured once by the top-level CMakeLists.
add_mod(deferred_fog
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(deferred_fog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/common)
