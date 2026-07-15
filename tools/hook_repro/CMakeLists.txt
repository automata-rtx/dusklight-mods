cmake_minimum_required(VERSION 3.25)
project(hook_repro CXX)

# Self-contained reproduction of the mod hook-resolution path. No game binary or import library is
# needed: the host stands in for the game, exports the hooked functions, and generates its own
# import library for the mod to link against (ENABLE_EXPORTS). Point DUSK_DIR at the pinned
# Dusklight source tree for the SDK headers.
set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../extern/dusklight" CACHE PATH
        "Path to the pinned Dusklight source tree")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Stand-in "game": exports hooked_free_fn* and, thanks to ENABLE_EXPORTS, produces host's import
# library so the mod DLL can link against it (mirrors linking against dusklight.exe's export lib).
add_executable(host host.cpp)
set_target_properties(host PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(host PRIVATE "${DUSK_DIR}/sdk/include")
target_link_libraries(host PRIVATE psapi)

# The test mod, built like a real mod: modmeta section, MultiThreadedDLL runtime, game feature,
# linked against the host's import library.
add_library(test_mod SHARED mod.cpp)
set_target_properties(test_mod PROPERTIES PREFIX "" OUTPUT_NAME "test_mod"
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_include_directories(test_mod PRIVATE "${DUSK_DIR}/sdk/include")
target_compile_definitions(test_mod PRIVATE DUSK_MOD_FEATURE_GAME _ITERATOR_DEBUG_LEVEL=0)
target_link_libraries(test_mod PRIVATE host)
