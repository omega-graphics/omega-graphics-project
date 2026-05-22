include(CMakeParseArguments)

function(add_d3d12_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;LIBS;ASSETS;SHADERS" ${ARGN})

    add_executable(${_ARG_NAME} WIN32 ${_ARG_SOURCES})
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS})

    add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${_ARG_NAME}> $<TARGET_FILE_DIR:${_ARG_NAME}>
        COMMAND_EXPAND_LISTS)

    foreach(_asset IN LISTS _ARG_ASSETS)
        if(NOT IS_ABSOLUTE "${_asset}")
            set(_asset_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}")
        else()
            set(_asset_abs "${_asset}")
        endif()
        get_filename_component(_asset_name "${_asset_abs}" NAME)
        add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_asset_abs}" "$<TARGET_FILE_DIR:${_ARG_NAME}>/${_asset_name}")
    endforeach()

    foreach(_shader IN LISTS _ARG_SHADERS)
        if(NOT IS_ABSOLUTE "${_shader}")
            set(_shader_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}")
        else()
            set(_shader_abs "${_shader}")
        endif()
        get_filename_component(_shader_basename "${_shader_abs}" NAME_WE)
        set(_shader_target "${_ARG_NAME}_${_shader_basename}_Shader")
        set(_shader_output "${CMAKE_CURRENT_BINARY_DIR}/${_shader_basename}.omegasllib")
        add_omegasl_lib("${_shader_target}" "${_shader_abs}" "${_shader_output}")
        add_dependencies(${_ARG_NAME} "${_shader_target}")
    endforeach()

    target_link_options(${_ARG_NAME} PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup /MANIFEST:NO)

endfunction()

add_d3d12_test(
    NAME
    "2DTest"
    SOURCES
    "2DTest/main.cpp"
    ASSETS
    "2DTest/test.png"
    SHADERS
    "2DTest/shaders.omegasl"
)

add_d3d12_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.cpp"
)

add_d3d12_test(
    NAME
    "GPUTessTest"
    SOURCES
    "GPUTessTest/main.cpp"
)

# Extension 8 §8.6 — runtime sampler-state binding integration test (shared,
# backend-independent source under gte/tests/).
add_d3d12_test(
    NAME
    "SamplerBindTest"
    SOURCES
    "../sampler_bind_test.cpp"
)
add_test(NAME omegagte_sampler_bind COMMAND SamplerBindTest)
# The negative case relies on a debug-build assertion firing; only register it
# (as a WILL_FAIL test) where asserts are active.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_test(NAME omegagte_sampler_bind_negative COMMAND SamplerBindTest negative)
    set_tests_properties(omegagte_sampler_bind_negative PROPERTIES WILL_FAIL TRUE)
endif()
