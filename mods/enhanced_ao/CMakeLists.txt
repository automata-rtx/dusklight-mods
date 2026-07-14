cmake_minimum_required(VERSION 3.25)
project(enhanced_ao CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to dusk source root")
    option(DUSK_MOD_USE_FULL_TREE "Use full build instead of the minimal mod SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

add_mod(enhanced_ao
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)

# Optional integration with the Depth to Normal provider mod (shared service header).
target_include_directories(enhanced_ao PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../depth_to_normal/include)
