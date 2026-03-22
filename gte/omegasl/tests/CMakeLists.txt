cmake_minimum_required(VERSION 3.9)

enable_testing()

set(OMEGASL_TEST_TEMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/temp")
file(MAKE_DIRECTORY "${OMEGASL_TEST_TEMP_DIR}")

# --- Helper: add a compile-only test for an .omegasl file ---
function(add_omegasl_compile_test TEST_NAME SOURCE_FILE)
    add_test(
        NAME ${TEST_NAME}
        COMMAND $<TARGET_FILE:omegaslc>
                -t ${OMEGASL_TEST_TEMP_DIR}
                -o ${OMEGASL_TEST_TEMP_DIR}/${TEST_NAME}.omegasllib
                ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}
    )
endfunction()

# --- Helper: add a negative test expected to fail ---
function(add_omegasl_fail_test TEST_NAME SOURCE_FILE)
    add_omegasl_compile_test(${TEST_NAME} ${SOURCE_FILE})
    set_tests_properties(${TEST_NAME} PROPERTIES WILL_FAIL TRUE)
endfunction()


# =============================================================
# 1. Tokenizer test
# =============================================================

add_test(
    NAME omegasl_tokens_shaders
    COMMAND $<TARGET_FILE:omegaslc>
            --tokens-only
            -t ${OMEGASL_TEST_TEMP_DIR}
            -o ${OMEGASL_TEST_TEMP_DIR}/tokens_dummy.omegasllib
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl
)

# =============================================================
# 2. Positive compilation tests
# =============================================================

add_omegasl_compile_test(omegasl_compile_shaders             shaders.omegasl)
add_omegasl_compile_test(omegasl_compile_operators           operators.omegasl)
add_omegasl_compile_test(omegasl_compile_control_flow        control_flow.omegasl)
add_omegasl_compile_test(omegasl_compile_vector_math         vector_math.omegasl)
add_omegasl_compile_test(omegasl_compile_resource_types      resource_types.omegasl)
add_omegasl_compile_test(omegasl_compile_compute_gradient    compute_gradient.omegasl)
add_omegasl_compile_test(omegasl_compile_user_functions      user_functions.omegasl)
add_omegasl_compile_test(omegasl_compile_const_fold          const_fold.omegasl)
add_omegasl_compile_test(omegasl_compile_tessellation        tessellation.omegasl)
add_omegasl_compile_test(omegasl_compile_math_builtins       math_builtins.omegasl)

# =============================================================
# 3. Negative compilation tests (expected to fail)
# =============================================================

add_omegasl_fail_test(omegasl_invalid_phase2_errors          invalid_phase2.omegasl)
add_omegasl_fail_test(omegasl_invalid_type_mismatch          invalid_type_mismatch.omegasl)
add_omegasl_fail_test(omegasl_invalid_undefined_resource     invalid_undefined_resource.omegasl)

# =============================================================
# 4. Golden-file tests for HLSL codegen (DirectX only)
# =============================================================

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
