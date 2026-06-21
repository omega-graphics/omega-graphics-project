
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

## add_omegasl_lib(<name> <src> <output> [INCLUDE_DIRS <dir>...])
##
## Compiles an OmegaSL source into a `.omegasllib` via omegaslc. The optional
## INCLUDE_DIRS keyword forwards each directory to omegaslc as an `-I <dir>`
## flag, so a quoted `#include "foo.omegaslh"` in the shader resolves against
## shared header locations (searched after the including file's own directory),
## letting several backends compile one canonical shader that pulls in shared
## headers. Omitting INCLUDE_DIRS keeps the original three-argument behavior, so
## existing call sites are unaffected.
function(add_omegasl_lib _NAME _SRC _OUTPUT)

    cmake_parse_arguments("_OSL" "" "" "INCLUDE_DIRS" ${ARGN})

    set(_include_flags)
    foreach(_dir IN LISTS _OSL_INCLUDE_DIRS)
        list(APPEND _include_flags "-I" "${_dir}")
    endforeach()

    add_custom_target(${_NAME} DEPENDS "${_OUTPUT}")

    make_directory(${CMAKE_CURRENT_BINARY_DIR}/omegasl)

    add_custom_command(OUTPUT "${_OUTPUT}"
                       COMMAND ${OMEGASLC_EXE} --temp-dir ${CMAKE_CURRENT_BINARY_DIR}/omegasl --output ${_OUTPUT} ${_include_flags} ${_SRC}
                       DEPENDS ${_SRC})

endfunction()
