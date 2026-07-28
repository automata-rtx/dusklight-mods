# Fetches the Dusklight source tree pinned by DUSKLIGHT_VERSION.
#
# Inputs:
#   DUSKLIGHT_VERSION       git tag or commit SHA to fetch (required unless DUSKLIGHT_DIR
#                           points at an existing checkout)
# Outputs / knobs:
#   DUSKLIGHT_DIR           Dusklight checkout, default <source>/dusklight. Point it at an
#                           existing checkout (e.g. a Dusklight development tree) to skip
#                           fetching entirely.
#   DUSKLIGHT_REPOSITORY    git remote to fetch from
#   DUSKLIGHT_AURORA_VERSION  optional extern/aurora commit to check out INSTEAD of the
#                           submodule's recorded pin, for when that pin is unreachable

include_guard(GLOBAL)

set(DUSKLIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dusklight"
        CACHE PATH "Path to the Dusklight source tree")
set(DUSKLIGHT_REPOSITORY "https://github.com/TwilitRealm/dusklight.git"
        CACHE STRING "Dusklight git repository to fetch from")

function(_exec_git_in _dir)
    list(JOIN ARGN " " _args_text)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
            WORKING_DIRECTORY "${_dir}"
            RESULT_VARIABLE _result)
    if (NOT _result EQUAL 0)
        message(FATAL_ERROR "'git ${_args_text}' failed in ${_dir}.\n"
                "Check that DUSKLIGHT_VERSION (${DUSKLIGHT_VERSION}) is a valid tag or commit "
                "in ${DUSKLIGHT_REPOSITORY} and that you are online.")
    endif ()
endfunction()

function(_exec_git)
    _exec_git_in("${DUSKLIGHT_DIR}" ${ARGN})
endfunction()

# A stamp file marks a checkout this module manages (and which version it holds). A checkout
# without one belongs to the user and is left untouched.
set(_dusklight_stamp "${DUSKLIGHT_DIR}/.stamp")

if (EXISTS "${DUSKLIGHT_DIR}/sdk/CMakeLists.txt" AND NOT EXISTS "${_dusklight_stamp}")
    message(STATUS "Dusklight: using existing checkout at ${DUSKLIGHT_DIR}")
else ()
    if (NOT DUSKLIGHT_VERSION)
        message(FATAL_ERROR "Dusklight: DUSKLIGHT_VERSION is not set")
    endif ()

    set(_dusklight_fetched "")
    if (EXISTS "${_dusklight_stamp}")
        file(READ "${_dusklight_stamp}" _dusklight_fetched)
        string(STRIP "${_dusklight_fetched}" _dusklight_fetched)
    endif ()

    if (NOT _dusklight_fetched STREQUAL DUSKLIGHT_VERSION)
        find_package(Git QUIET REQUIRED)
        message(STATUS "Dusklight: fetching ${DUSKLIGHT_VERSION} into ${DUSKLIGHT_DIR}")
        file(MAKE_DIRECTORY "${DUSKLIGHT_DIR}")
        if (NOT EXISTS "${DUSKLIGHT_DIR}/.git")
            _exec_git(init --quiet)
            _exec_git(remote add origin "${DUSKLIGHT_REPOSITORY}")
        endif ()
        # FetchContent's GIT_SHALLOW falls back to a full clone for SHAs, so we fetch
        # manually instead.
        _exec_git(fetch --depth 1 "${DUSKLIGHT_REPOSITORY}" "${DUSKLIGHT_VERSION}")
        _exec_git(-c advice.detachedHead=false checkout --force FETCH_HEAD)
        if (DUSKLIGHT_AURORA_VERSION)
            # Fork-local escape hatch: check out a specific extern/aurora commit instead of the
            # submodule's recorded pin, for when that pin is unreachable (a force-push can leave it
            # dangling, and `submodule update` then fails with "upload-pack: not our ref"). A mod
            # build consumes only aurora's Dawn/WebGPU headers, so a nearby commit is equivalent.
            # See the comment on DUSKLIGHT_AURORA_VERSION in the top-level CMakeLists.
            message(STATUS "Dusklight: overriding extern/aurora -> ${DUSKLIGHT_AURORA_VERSION}")
            _exec_git(submodule init extern/aurora)
            execute_process(
                    COMMAND "${GIT_EXECUTABLE}" config --get submodule.extern/aurora.url
                    WORKING_DIRECTORY "${DUSKLIGHT_DIR}"
                    OUTPUT_VARIABLE _dusklight_aurora_url
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
            set(_dusklight_aurora_dir "${DUSKLIGHT_DIR}/extern/aurora")
            file(MAKE_DIRECTORY "${_dusklight_aurora_dir}")
            if (NOT EXISTS "${_dusklight_aurora_dir}/.git")
                _exec_git_in("${_dusklight_aurora_dir}" init --quiet)
            endif ()
            _exec_git_in("${_dusklight_aurora_dir}"
                    fetch --depth 1 "${_dusklight_aurora_url}" "${DUSKLIGHT_AURORA_VERSION}")
            _exec_git_in("${_dusklight_aurora_dir}"
                    -c advice.detachedHead=false checkout --force FETCH_HEAD)
        else ()
            _exec_git(submodule update --init --depth 1 extern/aurora)
        endif ()
        file(WRITE "${_dusklight_stamp}" "${DUSKLIGHT_VERSION}\n")
    endif ()

    # Keep the stamp out of `git status` in the managed checkout.
    set(_dusklight_exclude "${DUSKLIGHT_DIR}/.git/info/exclude")
    set(_dusklight_exclude_text "")
    if (EXISTS "${_dusklight_exclude}")
        file(READ "${_dusklight_exclude}" _dusklight_exclude_text)
    endif ()
    if (NOT _dusklight_exclude_text MATCHES "(^|\n)/\\.stamp(\n|$)")
        file(APPEND "${_dusklight_exclude}" "/.stamp\n")
    endif ()

    # Shallow checkouts carry no tags for `git describe`; pin the SDK's version string instead.
    set(DUSK_VERSION_OVERRIDE "${DUSKLIGHT_VERSION}")
endif ()
