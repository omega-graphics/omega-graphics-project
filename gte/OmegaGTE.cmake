
# Promote `OMEGASLC_EXE` to a CACHE variable so the path is visible to
# sibling sub-projects that include this file's helpers (e.g. `wtk/` calls
# `add_omegasl_lib` from its own directory scope). Without `CACHE INTERNAL`
# the value would only live in `gte/`'s directory scope, and the function
# body's `${OMEGASLC_EXE}` would expand to empty when called from `wtk/`.
if(WIN32)
    set(OMEGASLC_EXE "${CMAKE_BINARY_DIR}/bin/omegaslc.exe" CACHE INTERNAL "Path to the omegaslc compiler executable")
else()
    set(OMEGASLC_EXE "${CMAKE_BINARY_DIR}/bin/omegaslc"     CACHE INTERNAL "Path to the omegaslc compiler executable")
endif()

## add_omegasl_lib(<name> <src> <output> [INCLUDE_DIRS <dir>...] [EXTRA_DEPENDS <file>...])
##
## Compiles an OmegaSL source into a `.omegasllib` via omegaslc. The optional
## INCLUDE_DIRS keyword forwards each directory to omegaslc as an `-I <dir>`
## flag, so a quoted `#include "foo.omegaslh"` in the shader resolves against
## shared header locations (searched after the including file's own directory),
## letting several backends compile one canonical shader that pulls in shared
## headers. EXTRA_DEPENDS lists additional files the compilation depends on —
## in practice the `.omegaslh` headers the source includes, so editing a
## shared header rebuilds every lib that consumes it. Omitting both keywords
## keeps the original three-argument behavior, so existing call sites are
## unaffected.
function(add_omegasl_lib _NAME _SRC _OUTPUT)

    cmake_parse_arguments("_OSL" "" "" "INCLUDE_DIRS;EXTRA_DEPENDS" ${ARGN})

    set(_include_flags)
    foreach(_dir IN LISTS _OSL_INCLUDE_DIRS)
        list(APPEND _include_flags "-I" "${_dir}")
    endforeach()

    # Record this source's include search dirs for the language server's
    # compile-command database (omegasl_commands.json — see
    # omega_write_omegasl_compile_commands below). Absolute, forward-slash paths
    # (CMake normalizes both), so the JSON strings never need escaping and match
    # the absolute path omegasl-lsp derives from a document's file:// URI.
    get_filename_component(_abs_src "${_SRC}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_dirs_json "")
    foreach(_dir IN LISTS _OSL_INCLUDE_DIRS)
        get_filename_component(_abs_dir "${_dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(_dirs_json STREQUAL "")
            set(_dirs_json "\"${_abs_dir}\"")
        else()
            set(_dirs_json "${_dirs_json},\"${_abs_dir}\"")
        endif()
    endforeach()
    list(APPEND OMEGASL_COMPILE_COMMANDS "{\"file\":\"${_abs_src}\",\"includeDirs\":[${_dirs_json}]}")
    set(OMEGASL_COMPILE_COMMANDS "${OMEGASL_COMPILE_COMMANDS}" CACHE INTERNAL
        "Accumulated OmegaSL compile commands (file + include dirs), one JSON object per add_omegasl_lib call.")

    add_custom_target(${_NAME} DEPENDS "${_OUTPUT}")

    make_directory(${CMAKE_CURRENT_BINARY_DIR}/omegasl)

    # `omegaslc` in DEPENDS is a target reference: CMake adds both the
    # target-level edge AND a file-level dependency on the compiler binary, so
    # a compiler fix recompiles every shader lib (a stale-lib bug the AQUA 5d
    # bring-up hit: the lexer fix landed but the kernels kept the old codegen).
    add_custom_command(OUTPUT "${_OUTPUT}"
                       COMMAND ${OMEGASLC_EXE} --temp-dir ${CMAKE_CURRENT_BINARY_DIR}/omegasl --output ${_OUTPUT} ${_include_flags} ${_SRC}
                       DEPENDS ${_SRC} ${_OSL_EXTRA_DEPENDS} omegaslc)

endfunction()

## add_omegasl_linked_lib(<name> <output> SOURCES <libA> <libB>...)
##
## Merges several already-built `.omegasllib` archives into one via
## `omegaslc --link` (a pure container merge — duplicate shader names and
## mismatched backends are rejected by the linker). This is how a module that
## splits its kernels across per-stage `.omegasl` sources (sharing helpers via
## an `.omegaslh` header) still ships ONE library for the runtime to load —
## AQUA's AQKernels.omegasllib is the canonical consumer.
function(add_omegasl_linked_lib _NAME _OUTPUT)

    cmake_parse_arguments("_OSL" "" "" "SOURCES" ${ARGN})

    get_filename_component(_libname "${_OUTPUT}" NAME_WE)

    add_custom_target(${_NAME} DEPENDS "${_OUTPUT}")

    add_custom_command(OUTPUT "${_OUTPUT}"
                       COMMAND ${OMEGASLC_EXE} --link ${_OSL_SOURCES} -o ${_OUTPUT} --lib-name ${_libname}
                       DEPENDS ${_OSL_SOURCES} omegaslc)

endfunction()

## omega_write_omegasl_compile_commands()
##
## Emit `${CMAKE_BINARY_DIR}/omegasl_commands.json` from the entries every
## `add_omegasl_lib` call accumulated into the `OMEGASL_COMPILE_COMMANDS` cache
## list. The file is the compile-command database `omegasl-lsp` reads to resolve
## a shader's `#include`s (each entry maps an absolute source path to its `-I`
## include dirs). Call this ONCE, at the end of the top-level CMakeLists.txt,
## after every add_subdirectory — so the list is fully populated when its
## content is expanded. The accumulator is reset once per configure at the top
## level (the cache list would otherwise persist stale entries across runs).
function(omega_write_omegasl_compile_commands)
    set(_json "[\n")
    set(_first TRUE)
    foreach(_entry IN LISTS OMEGASL_COMPILE_COMMANDS)
        if(_first)
            set(_json "${_json}  ${_entry}")
            set(_first FALSE)
        else()
            set(_json "${_json},\n  ${_entry}")
        endif()
    endforeach()
    set(_json "${_json}\n]\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/omegasl_commands.json" CONTENT "${_json}")
endfunction()
