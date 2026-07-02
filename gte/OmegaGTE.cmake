
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
