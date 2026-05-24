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
# Metal codegen + runtime have no tessellation pipeline yet, so the Metal
# backend rejects hull/domain stages with a clear diagnostic. The test is
# expected to fail on Apple builds — this verifies the diagnostic fires
# rather than that the (nonexistent) Metal pipeline works.
# See OmegaSL-Reference.md bug 3.
if(APPLE)
    set_tests_properties(omegasl_compile_tessellation PROPERTIES WILL_FAIL TRUE)
endif()
add_omegasl_compile_test(omegasl_compile_math_builtins       math_builtins.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_ops          matrix_ops.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_index        matrix_index.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_buffer       matrix_buffer.omegasl)
# Regression: builtin (scalar/vector) buffer element types.
add_omegasl_compile_test(omegasl_compile_buffer_scalar_elem  buffer_scalar_elem.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_write       texture_write.omegasl)
add_omegasl_compile_test(omegasl_compile_discard             discard.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_depth      fragment_depth.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_mrt        fragment_mrt.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_inputs     fragment_inputs.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_coverage   fragment_coverage.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_cube        texture_cube.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_array       texture_array.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_ms          texture_ms.omegasl)
add_omegasl_compile_test(omegasl_compile_sample_variants     sample_variants.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_gather      texture_gather.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_query       texture_query.omegasl)
add_omegasl_compile_test(omegasl_compile_index_assign        index_assign.omegasl)
add_omegasl_compile_test(omegasl_compile_switch_basic        switch_basic.omegasl)
add_omegasl_compile_test(omegasl_compile_math_phase5_1       math_phase5_1.omegasl)
# §5.2 — vector / matrix math (distance / faceforward / refract / inverse).
add_omegasl_compile_test(omegasl_compile_math_phase5_2       math_phase5_2.omegasl)
# §5.1.0 — modf / frexp out-param intrinsics + mod / mad aliases.
add_omegasl_compile_test(omegasl_compile_math_modf_frexp     math_modf_frexp.omegasl)
# §4.1 / §4.2 — 16-bit and 64-bit numeric type families.
add_omegasl_compile_test(omegasl_compile_numeric_16bit       numeric_16bit.omegasl)
add_omegasl_compile_test(omegasl_compile_numeric_64bit       numeric_64bit.omegasl)
# §3.2 — ternary `?:`.
add_omegasl_compile_test(omegasl_compile_ternary             ternary.omegasl)
# §3.6 — `const` qualifier on a local variable.
add_omegasl_compile_test(omegasl_compile_const_local_basic   const_local_basic.omegasl)
# §3.5 — function overloading.
add_omegasl_compile_test(omegasl_compile_function_overload_basic function_overload_basic.omegasl)
add_omegasl_compile_test(omegasl_compile_function_overload_with_forward function_overload_with_forward.omegasl)
# §3.7 — `out` / `inout` function parameters.
add_omegasl_compile_test(omegasl_compile_out_inout_params    out_inout_params.omegasl)
# §3.6 — `const` on function parameters + postfix `T const x` form.
add_omegasl_compile_test(omegasl_compile_const_param         const_param.omegasl)
# §3.6 — `const` local inside a nested (for/if/while/switch) block body.
add_omegasl_compile_test(omegasl_compile_nested_const        nested_const.omegasl)
# §2.4 — uniform / constant buffers.
add_omegasl_compile_test(omegasl_compile_uniform_buffer      uniform_buffer.omegasl)
# §6.1 — threadgroup / shared memory (incl. 2D shared-array indexing).
add_omegasl_compile_test(omegasl_compile_threadgroup_shared  threadgroup_shared.omegasl)
# §6.2 — compute barriers (threadgroupBarrier / deviceBarrier).
add_omegasl_compile_test(omegasl_compile_compute_barriers    compute_barriers.omegasl)

# --- Real-world shader tests (UI / game engine patterns) ---
add_omegasl_compile_test(omegasl_compile_blinn_phong         blinn_phong.omegasl)
add_omegasl_compile_test(omegasl_compile_gaussian_blur       gaussian_blur.omegasl)
add_omegasl_compile_test(omegasl_compile_sdf_shapes          sdf_shapes.omegasl)
add_omegasl_compile_test(omegasl_compile_pbr_metallic        pbr_metallic.omegasl)
add_omegasl_compile_test(omegasl_compile_post_process        post_process.omegasl)
add_omegasl_compile_test(omegasl_compile_particle_sim        particle_sim.omegasl)

# =============================================================
# 3. Negative compilation tests (expected to fail)
# =============================================================

add_omegasl_fail_test(omegasl_invalid_phase2_errors          invalid_phase2.omegasl)
add_omegasl_fail_test(omegasl_invalid_type_mismatch          invalid_type_mismatch.omegasl)
add_omegasl_fail_test(omegasl_invalid_undefined_resource     invalid_undefined_resource.omegasl)
add_omegasl_fail_test(omegasl_invalid_discard_outside_fragment invalid_discard_outside_fragment.omegasl)
add_omegasl_fail_test(omegasl_invalid_output_coverage_type   invalid_output_coverage_type.omegasl)
add_omegasl_fail_test(omegasl_invalid_sample_ms              invalid_sample_ms.omegasl)
add_omegasl_fail_test(omegasl_invalid_write_ms               invalid_write_ms.omegasl)
add_omegasl_fail_test(omegasl_invalid_sample_variants        invalid_sample_variants.omegasl)
add_omegasl_fail_test(omegasl_invalid_texture_query          invalid_texture_query.omegasl)
add_omegasl_fail_test(omegasl_invalid_modf_frexp             invalid_modf_frexp.omegasl)
# §5.2 — vector / matrix math diagnostics (arg counts; inverse non-square / non-matrix).
add_omegasl_fail_test(omegasl_invalid_math_phase5_2          invalid_math_phase5_2.omegasl)
add_omegasl_fail_test(omegasl_invalid_switch                 invalid_switch.omegasl)
add_omegasl_fail_test(omegasl_invalid_matrix_column_write    invalid_matrix_column_write.omegasl)
# §3.2 — ternary diagnostics.
add_omegasl_fail_test(omegasl_invalid_ternary_type_mismatch  invalid_ternary_type_mismatch.omegasl)
add_omegasl_fail_test(omegasl_invalid_ternary_nonbool_cond   invalid_ternary_nonbool_cond.omegasl)
# §3.6 — `const` local enforcement diagnostics.
add_omegasl_fail_test(omegasl_invalid_const_assign           invalid_const_assign.omegasl)
add_omegasl_fail_test(omegasl_invalid_const_no_init          invalid_const_no_init.omegasl)
add_omegasl_fail_test(omegasl_invalid_const_increment        invalid_const_increment.omegasl)
# §3.6 — `const` parameter diagnostics (const+out/inout, write-through, dup const).
add_omegasl_fail_test(omegasl_invalid_const_out_param        invalid_const_out_param.omegasl)
add_omegasl_fail_test(omegasl_invalid_const_param_assign     invalid_const_param_assign.omegasl)
add_omegasl_fail_test(omegasl_invalid_dup_const              invalid_dup_const.omegasl)
# §3.5 — overload-resolution diagnostics.
add_omegasl_fail_test(omegasl_invalid_function_overload_no_match     invalid_function_overload_no_match.omegasl)
add_omegasl_fail_test(omegasl_invalid_function_overload_redecl       invalid_function_overload_redecl.omegasl)
add_omegasl_fail_test(omegasl_invalid_function_overload_return_type  invalid_function_overload_return_type.omegasl)
# §2.4 — uniform diagnostics (indexing / out-access / non-struct element).
add_omegasl_fail_test(omegasl_invalid_uniform                invalid_uniform.omegasl)
# Regression: `buffer` with no element type must diagnose, not crash.
add_omegasl_fail_test(omegasl_invalid_buffer_no_args         invalid_buffer_no_args.omegasl)
# §6.1 — threadgroup diagnostics (outside compute / initializer).
add_omegasl_fail_test(omegasl_invalid_threadgroup_outside_compute invalid_threadgroup_outside_compute.omegasl)
add_omegasl_fail_test(omegasl_invalid_threadgroup_init       invalid_threadgroup_init.omegasl)
# §6.2 — barrier diagnostics (outside compute / arg-count).
add_omegasl_fail_test(omegasl_invalid_barrier_outside_compute invalid_barrier_outside_compute.omegasl)
add_omegasl_fail_test(omegasl_invalid_barrier_args           invalid_barrier_args.omegasl)
# §5.1.0 follow-up — reserved builtin intrinsic name as a user function.
add_omegasl_fail_test(omegasl_invalid_reserved_name          invalid_reserved_name.omegasl)

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
