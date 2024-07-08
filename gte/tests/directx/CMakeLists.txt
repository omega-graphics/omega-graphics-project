include(CMakeParseArguments)

function(add_d3d12_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;LIBS" ${ARGN})
    
    add_executable(${_ARG_NAME} WIN32 ${_ARG_SOURCES})
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS})
    add_custom_command(TARGET ${_ARG_NAME} POST_BUILD 
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bin/OmegaGTE.dll ${CMAKE_CURRENT_BINARY_DIR}/OmegaGTE.dll)
    
endfunction()

add_omegasl_lib("2DTestShaders" "${CMAKE_CURRENT_SOURCE_DIR}/2DTest/shaders.omegasl" "${CMAKE_CURRENT_BINARY_DIR}/shaders.omegasllib")


add_d3d12_test(
    NAME 
    "2DTest"
    SOURCES
    "2DTest/main.cpp"
)
