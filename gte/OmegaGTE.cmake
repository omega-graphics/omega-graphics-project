
set(OMEGASLC_EXE ${CMAKE_BINARY_DIR}/bin/omegaslc)
if(WIN32)
    set(OMEGASLC_EXE ${OMEGASLC_EXE}.exe)
endif()

function(add_omegasl_lib _NAME _SRC _OUTPUT)

    add_custom_target(${_NAME} DEPENDS "${_OUTPUT}")
    
    add_custom_command(OUTPUT "${_OUTPUT}"
                       COMMAND ${OMEGASLC_EXE} --temp-dir ${CMAKE_CURRENT_BINARY_DIR}/omegasl --output ${_OUTPUT} ${_SRC}
                       DEPENDS ${_SRC})
    
endfunction()
