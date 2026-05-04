
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

function(add_omegasl_lib _NAME _SRC _OUTPUT)

    add_custom_target(${_NAME} DEPENDS "${_OUTPUT}")

    make_directory(${CMAKE_CURRENT_BINARY_DIR}/omegasl)

    add_custom_command(OUTPUT "${_OUTPUT}"
                       COMMAND ${OMEGASLC_EXE} --temp-dir ${CMAKE_CURRENT_BINARY_DIR}/omegasl --output ${_OUTPUT} ${_SRC}
                       DEPENDS ${_SRC})

endfunction()
