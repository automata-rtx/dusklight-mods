# ReShade Bridge: lets an installed ReShade run its effects inside Dusklight's frame (see
# docs/reshade_bridge.md). Two binaries from one source tree:
#
#   reshade_bridge (.dusk)              The Dusklight mod. Game-linked (one post-hook gives the
#                                       "before particles" insertion point) + webgpu. Builds on
#                                       every platform; it only finds the add-on on Windows.
#   dusklight_reshade_bridge (.addon64) The ReShade add-on. Windows x64 only (ReShade's add-on API
#                                       has no other target that Dusklight runs on). Lands next to
#                                       the .dusk in ${CMAKE_BINARY_DIR}/mods so CI uploads both.
#
# include/drb_protocol.hpp is the contract between the two; both build from this one tree in the
# same CI run, so the protocol only has to agree with itself.

add_mod(reshade_bridge
    FEATURES game webgpu
    SOURCES  src/mod.cpp
             src/bridge_gpu.cpp
             src/bridge_link.cpp
             src/game_hooks.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(reshade_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/common)

# The add-on. ReShade loads *.addon64 files from its add-on search path (by default the folder that
# holds ReShade's DLL) and only does so in builds "with full add-on support".
#
# Static CRT so the add-on has no runtime dependency beyond the system DLLs. Plain MSVC, like the
# rest of the repository. CMAKE_CXX_COMPILER_ARCHITECTURE_ID rather than CMAKE_SYSTEM_PROCESSOR: the
# windows-arm64 CI leg cross-compiles from an x64 host.
if (MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    add_library(dusklight_reshade_bridge SHARED
        addon/bridge_addon.cpp
        addon/state_restore.cpp)
    target_include_directories(dusklight_reshade_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/addon
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    target_compile_features(dusklight_reshade_bridge PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_reshade_bridge PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(dusklight_reshade_bridge PRIVATE /utf-8 /W3)
    set_target_properties(dusklight_reshade_bridge PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon-lib")
    # No import libraries: the add-on only calls D3D12 through interfaces it is handed, and must not
    # import d3d12.dll or dxgi.dll itself (either may be ReShade's own proxy DLL).
endif ()
