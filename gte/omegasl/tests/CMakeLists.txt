cmake_minimum_required(VERSION 3.9)

enable_testing()

set(OMEGASL_TEST_TEMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/temp")
file(MAKE_DIRECTORY "${OMEGASL_TEST_TEMP_DIR}")

# 5.1 Compiler unit tests: basic end-to-end and error cases

add_test(
    NAME omegasl_tokens_shaders
    COMMAND $<TARGET_FILE:omegaslc>
            --tokens-only
            -t ${OMEGASL_TEST_TEMP_DIR}
            -o ${OMEGASL_TEST_TEMP_DIR}/tokens_dummy.omegasllib
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
)

add_test(
    NAME omegasl_compile_shaders
    COMMAND $<TARGET_FILE:omegaslc>
            -t ${OMEGASL_TEST_TEMP_DIR}
            -o ${OMEGASL_TEST_TEMP_DIR}/shaders.omegasllib
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
)

add_test(
    NAME omegasl_invalid_phase2_errors
    COMMAND $<TARGET_FILE:omegaslc>
            -t ${OMEGASL_TEST_TEMP_DIR}
            -o ${OMEGASL_TEST_TEMP_DIR}/invalid.omegasllib
            ${CMAKE_CURRENT_SOURCE_DIR}/invalid_phase2.omegasl
)

# This test is expected to fail compilation and produce diagnostics.
set_tests_properties(omegasl_invalid_phase2_errors PROPERTIES WILL_FAIL TRUE)

# 5.2 Golden-file tests for HLSL codegen (only when DirectX backend is available)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/golden")

if(TARGET_DIRECTX)
    set(GOLDEN_OUT_DIR "${OMEGASL_TEST_TEMP_DIR}/golden_out")

    add_test(
        NAME omegasl_hlsl_golden_myVertex
        COMMAND ${CMAKE_COMMAND}
                -DOMEGASLC=$<TARGET_FILE:omegaslc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
                -DOUTDIR=${GOLDEN_OUT_DIR}
                -DOUTPUT_NAME=myVertex
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden/myVertex.hlsl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunGoldenCodegenTest.cmake
    )

    add_test(
        NAME omegasl_hlsl_golden_myFrag
        COMMAND ${CMAKE_COMMAND}
                -DOMEGASLC=$<TARGET_FILE:omegaslc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
                -DOUTDIR=${GOLDEN_OUT_DIR}
                -DOUTPUT_NAME=myFrag
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden/myFrag.hlsl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunGoldenCodegenTest.cmake
    )

    add_test(
        NAME omegasl_hlsl_golden_myVertex2
        COMMAND ${CMAKE_COMMAND}
                -DOMEGASLC=$<TARGET_FILE:omegaslc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
                -DOUTDIR=${GOLDEN_OUT_DIR}
                -DOUTPUT_NAME=myVertex2
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden/myVertex2.hlsl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunGoldenCodegenTest.cmake
    )

    add_test(
        NAME omegasl_hlsl_golden_myFrag2
        COMMAND ${CMAKE_COMMAND}
                -DOMEGASLC=$<TARGET_FILE:omegaslc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
                -DOUTDIR=${GOLDEN_OUT_DIR}
                -DOUTPUT_NAME=myFrag2
                -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden/myFrag2.hlsl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunGoldenCodegenTest.cmake
    )
endif()

