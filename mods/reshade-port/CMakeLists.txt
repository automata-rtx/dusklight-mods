# ReShade Port — runs ReShade effect files (.fx) at chosen points of the game's frame (see
# docs/reshade_port.md). Game-linked: one post-hook (src/game_hooks.cpp) provides the insertion point
# between the world's translucent geometry and the game's post-processing; everything else uses
# GfxService. The mod SDK is configured once by the top-level CMakeLists.
#
# third_party/reshadefx is ReShade's own effect compiler (BSD-3-Clause, VENDORED.md lists the
# version and our patches); it builds as a static library with the same visibility and char
# signedness as the mod. third_party/stb provides image loading and resizing (public domain).

file(GLOB _reshadefx_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshadefx/*.cpp)
add_library(reshade_port_reshadefx STATIC ${_reshadefx_sources})
target_include_directories(reshade_port_reshadefx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshadefx)
target_compile_features(reshade_port_reshadefx PRIVATE cxx_std_20)
set_target_properties(reshade_port_reshadefx PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if (MSVC)
    target_compile_options(reshade_port_reshadefx PRIVATE /utf-8 /bigobj)
else ()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _rsp_arch)
    if (_rsp_arch MATCHES "^(arm|aarch64)")
        target_compile_options(reshade_port_reshadefx PRIVATE -fsigned-char)
    endif ()
endif ()

find_package(Threads REQUIRED)

add_mod(reshade_port
    FEATURES game webgpu
    SOURCES  src/mod.cpp
             src/game_hooks.cpp
             src/codegen_wgsl.cpp
             src/fx_compile.cpp
             src/fx_gpu.cpp
             src/fx_images.cpp
             src/fx_preset.cpp
             src/fx_runtime.cpp
             src/fx_uniforms.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(reshade_port PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(reshade_port PRIVATE reshade_port_reshadefx Threads::Threads)
