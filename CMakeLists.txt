cmake_minimum_required(VERSION 3.25)
project(dusklight_mods CXX)

# Superproject for our Dusklight graphics mods. Configures the Dusklight mod SDK
# (headers + a prebuilt Dawn package - no game or aurora compilation) from the pinned
# submodule, then builds each mod as a .dusk package into ${CMAKE_BINARY_DIR}/mods.
#
# Windows additionally needs the import library from the matching game release, and clang-cl:
#   -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   -DDUSK_GAME_EXE=<path to windows-amd64.lib>   (platform-v2-test release asset)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/dusklight" CACHE PATH
        "Path to the pinned Dusklight source tree")
add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)

add_subdirectory(mods/deferred_fog)
add_subdirectory(mods/depth_to_normal)
add_subdirectory(mods/enhanced_ao)
add_subdirectory(mods/realtime_sun_shadows)

# Offline WGSL validator (Dawn Null backend). Used by CI and local shader iteration:
#   cmake -B build -DMODS_BUILD_TOOLS=ON && cmake --build build --target wgsl_validate
#   build/wgsl_validate mods/enhanced_ao/res/vbao.wgsl
option(MODS_BUILD_TOOLS "Build repo tools (WGSL validator)" OFF)
if (MODS_BUILD_TOOLS)
    add_executable(wgsl_validate tools/wgsl_validate.cpp)
    target_compile_features(wgsl_validate PRIVATE cxx_std_20)
    target_link_libraries(wgsl_validate PRIVATE dawn::webgpu_dawn)
    if (TARGET dawn::dawncpp_headers)
        target_link_libraries(wgsl_validate PRIVATE dawn::dawncpp_headers)
    endif ()
endif ()
