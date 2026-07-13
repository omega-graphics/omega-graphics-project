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

# --- Helper: add a test asserting omegaslc emits a diagnostic matching REGEX ---
# PASS_REGULAR_EXPRESSION passes the test when the regex matches the output
# regardless of exit code, so it asserts the *message*; pair it with an
# add_omegasl_fail_test on the same source to also assert the nonzero exit.
function(add_omegasl_diag_test TEST_NAME SOURCE_FILE REGEX)
    add_omegasl_compile_test(${TEST_NAME} ${SOURCE_FILE})
    set_tests_properties(${TEST_NAME} PROPERTIES PASS_REGULAR_EXPRESSION "${REGEX}")
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
add_omegasl_compile_test(omegasl_compile_dot_cross_operand   dot_cross_operand.omegasl)
add_omegasl_compile_test(omegasl_compile_sci_notation        sci_notation_literal.omegasl)
add_omegasl_compile_test(omegasl_compile_precedence          precedence_unary_ternary.omegasl)
add_omegasl_compile_test(omegasl_compile_const_fold          const_fold.omegasl)
add_omegasl_compile_test(omegasl_compile_tessellation        tessellation.omegasl)
# §16 — tessellation now compiles end-to-end on ALL THREE backends, so this
# test is expected-PASS everywhere (no WILL_FAIL):
#   - Metal (§16-D): hull → compute kernel (per-CP store + patchfn factor
#     epilogue), domain → `[[patch(...)]]` post-tess vertex; compiled by `metal`.
#   - HLSL (§16-B): hull → control-point entry + `[patchconstantfunc]` naming
#     the patchfn (`SV_TessFactor`/`SV_InsideTessFactor`), `InputPatch<CP,N>` /
#     `SV_OutputControlPointID`; domain → `OutputPatch<CP,N>` /
#     `SV_DomainLocation`; compiled by `dxc`.
#   - GLSL (§16-C): `.tesc` writes `gl_out[]` + `gl_TessLevelOuter/Inner[]`
#     (from the patchfn, guarded on `gl_InvocationID == 0`); `.tese` reads
#     `gl_TessCoord` for the DomainLocation; compiled by `glslc`.
# The patch-constant struct is emitted as a plain data struct on every backend.
# OmegaSL-Reference.md bugs 3 (Metal) and 4 (HLSL) are both resolved.
add_omegasl_compile_test(omegasl_compile_math_builtins       math_builtins.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_ops          matrix_ops.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_index        matrix_index.omegasl)
add_omegasl_compile_test(omegasl_compile_matrix_buffer       matrix_buffer.omegasl)
add_omegasl_compile_test(omegasl_compile_int_matrix          int_matrix.omegasl)
# Regression: builtin (scalar/vector) buffer element types.
add_omegasl_compile_test(omegasl_compile_buffer_scalar_elem  buffer_scalar_elem.omegasl)
add_omegasl_compile_test(omegasl_compile_texture_write       texture_write.omegasl)
add_omegasl_compile_test(omegasl_compile_discard             discard.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_depth      fragment_depth.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_mrt        fragment_mrt.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_inputs     fragment_inputs.omegasl)
add_omegasl_compile_test(omegasl_compile_fragment_coverage   fragment_coverage.omegasl)
# §1.5 — early depth/stencil (`fragment(early_depth)`) on a UAV-writing fragment.
add_omegasl_compile_test(omegasl_compile_early_depth         early_depth.omegasl)
# §1.6 — interpolation modifiers (flat / centroid / sample / noperspective).
add_omegasl_compile_test(omegasl_compile_interp_modifiers    interp_modifiers.omegasl)
# §1.7 — clip / cull distance (float-array varyings; CullDistance gated on MSL).
add_omegasl_compile_test(omegasl_compile_clip_cull_distance  clip_cull_distance.omegasl)
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
# §5.2 — bool vectors + component-wise compare + any/all.
add_omegasl_compile_test(omegasl_compile_bool_vector         bool_vector.omegasl)
# §5.3 — integer / bitfield ops (Phase A: countbits / reversebits).
add_omegasl_compile_test(omegasl_compile_bitfield_ops        bitfield_ops.omegasl)
# §5.4 — fragment-stage screen-space derivatives (ddx/ddy/fwidth + coarse/fine).
add_omegasl_compile_test(omegasl_compile_derivatives         derivatives.omegasl)
# §5.5 Phase A — bit-pattern reinterpret (asint / asuint / asfloat).
add_omegasl_compile_test(omegasl_compile_bit_reinterpret     bit_reinterpret.omegasl)
# §5.5 Phase B — half-float pack / unpack (f16tof32 / f32tof16 + packed).
add_omegasl_compile_test(omegasl_compile_half_pack           half_pack.omegasl)
# §5.5 Phase C — normalized 4x8 / 2x16 pack / unpack family.
add_omegasl_compile_test(omegasl_compile_pack_norm           pack_norm.omegasl)
# §5.6 Phase A — atomic ops (add/min/max/and/or/xor/exchange + load/store).
add_omegasl_compile_test(omegasl_compile_atomic_ops          atomic_ops.omegasl)
# Raytracing plan §1-2 — inline ray query: Ray/RayHit/AccelerationStructure
# builtin types + intersect() lowered to inline ray query on every backend.
add_omegasl_compile_test(omegasl_compile_inline_raytracing   inline_raytracing.omegasl)
# Raytracing plan §1.5 — low-level RayQuery traversal loop (ray_query_*).
add_omegasl_compile_test(omegasl_compile_ray_query_loop      ray_query_loop.omegasl)
# §5.1.0 — modf / frexp out-param intrinsics + mod / mad aliases.
add_omegasl_compile_test(omegasl_compile_math_modf_frexp     math_modf_frexp.omegasl)
# §4.1 / §4.2 / §4.3 — 16-bit, 64-bit, and double numeric type families.
add_omegasl_compile_test(omegasl_compile_numeric_16bit       numeric_16bit.omegasl)
add_omegasl_compile_test(omegasl_compile_numeric_64bit       numeric_64bit.omegasl)
# §4.3 — `double` (DOUBLE-gated; HLSL/GLSL express it, MSL stubs the shader).
add_omegasl_compile_test(omegasl_compile_numeric_double      numeric_double.omegasl)
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
# §2.2/§10.2 — push constants (constant<T>).
add_omegasl_compile_test(omegasl_compile_push_constant       push_constant.omegasl)
# §6.1 — threadgroup / shared memory (incl. 2D shared-array indexing).
add_omegasl_compile_test(omegasl_compile_threadgroup_shared  threadgroup_shared.omegasl)
# §6.2 — compute barriers (threadgroupBarrier / deviceBarrier).
add_omegasl_compile_test(omegasl_compile_compute_barriers    compute_barriers.omegasl)

# Phase 5/6 — `.omegaslh` headers: a translation unit may `#include` a header
# of shared declarations (struct + buffer resources + helper func) and use
# them. The comment-guard header names stage keywords only inside comments
# and as an identifier substring, proving the shader-rejection scan is
# token-based (not substring-based) by still compiling clean.
add_omegasl_compile_test(omegasl_compile_clean_header        include_clean_header.omegasl)
add_omegasl_compile_test(omegasl_compile_comment_guard       include_comment_guard.omegasl)
# Phase 4 — including a non-`.omegaslh` file as a header is warn-only: the
# compile test asserts it still succeeds (exit 0); the diag test asserts the
# advisory warning text is emitted.
add_omegasl_compile_test(omegasl_compile_non_omegaslh_include include_non_omegaslh_header.omegasl)
add_omegasl_diag_test(omegasl_warn_non_omegaslh_ext          include_non_omegaslh_header.omegasl
                      "does not use the recommended")

# `-I` / `--include-dir` search path — include a header that lives in the
# sibling `include/` subdirectory, NOT next to the translation unit. The
# positive test passes `-I <tests>/include` and must compile clean; the
# negative test omits the flag, so the include can't resolve, the helper /
# resources go undefined, and the compile fails. The pair proves the driver
# wires `-I` through to the preprocessor and that the dir is load-bearing.
add_test(
    NAME omegasl_include_search_dir
    COMMAND $<TARGET_FILE:omegaslc>
            -t ${OMEGASL_TEST_TEMP_DIR}
            -o ${OMEGASL_TEST_TEMP_DIR}/omegasl_include_search_dir.omegasllib
            -I ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include_search_dir.omegasl
)
add_omegasl_fail_test(omegasl_include_search_dir_no_flag     include_search_dir.omegasl)

# A `#include` that resolves to no file in any search dir is no longer
# dropped silently — it fails loud with a "cannot find included file"
# diagnostic naming every directory searched. The fail test asserts the
# nonzero exit; the diag test asserts the message.
add_omegasl_fail_test(omegasl_include_missing_header         include_missing_header.omegasl)
add_omegasl_diag_test(omegasl_include_missing_header_diag    include_missing_header.omegasl
                      "cannot find included file")
# The unsupported angle-bracket include form is diagnosed as malformed
# rather than silently ignored.
add_omegasl_fail_test(omegasl_include_angle_bracket          include_angle_bracket.omegasl)
add_omegasl_diag_test(omegasl_include_angle_bracket_diag     include_angle_bracket.omegasl
                      "expected a quoted path")

# --- Real-world shader tests (UI / game engine patterns) ---
add_omegasl_compile_test(omegasl_compile_blinn_phong         blinn_phong.omegasl)
add_omegasl_compile_test(omegasl_compile_gaussian_blur       gaussian_blur.omegasl)
add_omegasl_compile_test(omegasl_compile_sdf_shapes          sdf_shapes.omegasl)
add_omegasl_compile_test(omegasl_compile_pbr_metallic        pbr_metallic.omegasl)
add_omegasl_compile_test(omegasl_compile_post_process        post_process.omegasl)
add_omegasl_compile_test(omegasl_compile_particle_sim        particle_sim.omegasl)

# Mesh-shader pipeline (Mesh-Shader-Implementation-Plan.md).
#
# `mesh_basic` predates these registrations — Phases 2a/2b/2c verified it by
# hand in-session and never wired it into CTest, so the mesh codegen path had no
# standing regression coverage at all. Registering it here closes that gap.
#
# `amplification_basic` (§5) is the two-stage amp -> mesh pipeline: an
# amplification shader fills a payload and dispatches a computed number of mesh
# threadgroups, each of which reads the payload back.
add_omegasl_compile_test(omegasl_compile_mesh_basic           mesh_basic.omegasl)
add_omegasl_compile_test(omegasl_compile_amplification_basic amplification_basic.omegasl)

# =============================================================
# 3. Negative compilation tests (expected to fail)
# =============================================================

# Phase 5/6 — a shader entry point declared in an `#include`d header is
# rejected. The fail test asserts the nonzero exit (guards the main.cpp
# preprocessor-error gate); the diag test asserts the diagnostic text.
add_omegasl_fail_test(omegasl_reject_shader_in_header        include_shader_in_header.omegasl)
add_omegasl_diag_test(omegasl_reject_shader_in_header_diag   include_shader_in_header.omegasl
                      "declares a shader entry point")

add_omegasl_fail_test(omegasl_invalid_phase2_errors          invalid_phase2.omegasl)
add_omegasl_fail_test(omegasl_invalid_type_mismatch          invalid_type_mismatch.omegasl)
add_omegasl_fail_test(omegasl_invalid_undefined_resource     invalid_undefined_resource.omegasl)
add_omegasl_fail_test(omegasl_invalid_discard_outside_fragment invalid_discard_outside_fragment.omegasl)
# §1.5 — unknown fragment descriptor token must be diagnosed, not ignored.
add_omegasl_fail_test(omegasl_invalid_early_depth            invalid_early_depth.omegasl)
# §1.6 — integer varying without `flat` is rejected.
add_omegasl_fail_test(omegasl_invalid_interp_modifier        invalid_interp_modifier.omegasl)
# §1.7 — clip/cull distance: non-float element + indexed form are rejected.
add_omegasl_fail_test(omegasl_invalid_clip_cull              invalid_clip_cull.omegasl)
# §16 — a hull must name a patch-constant function (`patchfn=`).
add_omegasl_fail_test(omegasl_invalid_tessellation           invalid_tessellation.omegasl)
add_omegasl_fail_test(omegasl_invalid_output_coverage_type   invalid_output_coverage_type.omegasl)
add_omegasl_fail_test(omegasl_invalid_sample_ms              invalid_sample_ms.omegasl)
add_omegasl_fail_test(omegasl_invalid_write_ms               invalid_write_ms.omegasl)
add_omegasl_fail_test(omegasl_invalid_sample_variants        invalid_sample_variants.omegasl)
add_omegasl_fail_test(omegasl_invalid_texture_query          invalid_texture_query.omegasl)
add_omegasl_fail_test(omegasl_invalid_modf_frexp             invalid_modf_frexp.omegasl)
# §5.2 — vector / matrix math diagnostics (arg counts; inverse non-square / non-matrix).
add_omegasl_fail_test(omegasl_invalid_math_phase5_2          invalid_math_phase5_2.omegasl)
# §5.2 — bool-vector diagnostics (any/all on non-bvec; compare arg count / mismatch / non-vector).
add_omegasl_fail_test(omegasl_invalid_bool_vector            invalid_bool_vector.omegasl)
# §5.3 — bitfield-op diagnostics (float operand; arg count).
add_omegasl_fail_test(omegasl_invalid_bitfield_ops           invalid_bitfield_ops.omegasl)
# §5.4 — derivative diagnostics (stage, type, arg count).
add_omegasl_fail_test(omegasl_invalid_derivatives            invalid_derivatives.omegasl)
# §5.5 Phase A — bit-reinterpret diagnostics (arg count, 16-bit, bool, matrix).
add_omegasl_fail_test(omegasl_invalid_bit_reinterpret        invalid_bit_reinterpret.omegasl)
# §5.5 Phase B — half-pack diagnostics (arg count + per-builtin operand type).
add_omegasl_fail_test(omegasl_invalid_half_pack              invalid_half_pack.omegasl)
# §5.5 Phase C — normalized-pack diagnostics (arg count + per-builtin operand type).
add_omegasl_fail_test(omegasl_invalid_pack_norm              invalid_pack_norm.omegasl)
# §5.6 — atomic op diagnostics (non-atomic operand / type mismatch / arg count).
add_omegasl_fail_test(omegasl_invalid_atomic_ops             invalid_atomic_ops.omegasl)
add_omegasl_fail_test(omegasl_invalid_switch                 invalid_switch.omegasl)
add_omegasl_fail_test(omegasl_invalid_matrix_column_write    invalid_matrix_column_write.omegasl)
# Matrix index-type rule (column + two-level row index must be int/uint).
add_omegasl_fail_test(omegasl_invalid_matrix_index           invalid_matrix_index.omegasl)
# §12.2 follow-up — integer matrices reject whole-matrix ops and inline ctors.
add_omegasl_fail_test(omegasl_invalid_int_matrix_op          invalid_int_matrix_op.omegasl)
add_omegasl_fail_test(omegasl_invalid_int_matrix_ctor        invalid_int_matrix_ctor.omegasl)
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
add_omegasl_fail_test(omegasl_invalid_push_constant          invalid_push_constant.omegasl)
add_omegasl_fail_test(omegasl_invalid_duplicate_binding      invalid_duplicate_binding.omegasl)
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
# Internal (shader-I/O) struct fields must carry a semantic attribute — an
# unattributed field is expressible only on GLSL, so Sema rejects it for all
# backends.
add_omegasl_fail_test(omegasl_invalid_internal_unattributed_field invalid_internal_unattributed_field.omegasl)

# --- Mesh-shader pipeline (Mesh-Shader-Implementation-Plan.md) --------------
# These fixtures also predate their registration: §2a/§2b landed them and
# verified them by hand, so like `mesh_basic` above they had no standing CTest
# coverage. Wired up here alongside the §5 amplification negatives.
#
# §2b — `topology=point` is a portability gate, not a capability gap: GLSL and
# Metal support it natively, HLSL SM 6.5 has no `[outputtopology("point")]`.
add_omegasl_fail_test(omegasl_invalid_topology_point         invalid_topology_point.omegasl)
# §2a follow-up — `setMeshOutputs` diagnostics (stage / types / duplicate / bounds).
add_omegasl_fail_test(omegasl_invalid_set_mesh_outputs_outside_mesh invalid_set_mesh_outputs_outside_mesh.omegasl)
add_omegasl_fail_test(omegasl_invalid_set_mesh_outputs_wrong_types  invalid_set_mesh_outputs_wrong_types.omegasl)
add_omegasl_fail_test(omegasl_invalid_set_mesh_outputs_duplicate    invalid_set_mesh_outputs_duplicate.omegasl)
add_omegasl_fail_test(omegasl_invalid_set_mesh_outputs_exceeds_max  invalid_set_mesh_outputs_exceeds_max.omegasl)
# §5 — amplification-stage diagnostics. The `dispatchMesh` must-be-last rule is
# the interesting one: it is the INTERSECTION of the three backends' rules, not
# any one backend's, so the diag test pins the message that explains why (GLSL's
# EmitMeshTasksEXT terminates the shader; HLSL and Metal would accept the
# trailing statement). Without that reasoning in the message the rule reads as
# arbitrary pedantry to whoever trips it.
add_omegasl_fail_test(omegasl_invalid_dispatch_mesh_not_last    invalid_dispatch_mesh_not_last.omegasl)
add_omegasl_diag_test(omegasl_invalid_dispatch_mesh_not_last_diag invalid_dispatch_mesh_not_last.omegasl
                      "must be the final statement of the body")
add_omegasl_fail_test(omegasl_invalid_dispatch_mesh_outside_amp invalid_dispatch_mesh_outside_amp.omegasl)
add_omegasl_diag_test(omegasl_invalid_dispatch_mesh_outside_amp_diag invalid_dispatch_mesh_outside_amp.omegasl
                      "only valid inside an amplification shader")
add_omegasl_fail_test(omegasl_invalid_amp_no_payload            invalid_amp_no_payload.omegasl)
add_omegasl_fail_test(omegasl_invalid_payload_wrong_direction   invalid_payload_wrong_direction.omegasl)
add_omegasl_diag_test(omegasl_invalid_payload_wrong_direction_diag invalid_payload_wrong_direction.omegasl
                      "use `in payload`")

# =============================================================
# 4. ShaderArchive Read->Write byte-identical round-trip (Phase 1)
# =============================================================

# Host-only test: compiles the shared (de)serializer directly (no GPU device)
# and asserts ReadShaderArchive / WriteShaderArchive are exact inverses on a
# real generated lib — the regression guard that keeps the single owner of the
# `.omegasllib` byte layout honest. Mirrors VulkanQueueFamiliesTest's pattern
# of building an in-tree translation unit into a standalone host test.
add_executable(ArchiveRoundTripTest
    archive_roundtrip_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ShaderArchive.cpp)
target_include_directories(ArchiveRoundTripTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include)
target_compile_definitions(ArchiveRoundTripTest PRIVATE ${PUBLIC_DEFS})
target_link_libraries(ArchiveRoundTripTest PRIVATE OmegaCommonCore)

# Generate a fixture lib (vertex + fragment + resource layout coverage) as a
# CTest fixture, then run the round-trip on it. FIXTURES_* means running the
# round-trip alone (`ctest -R omegasl_archive_roundtrip`) still produces its
# input first.
set(ARCHIVE_FIXTURE_LIB "${OMEGASL_TEST_TEMP_DIR}/archive_roundtrip_fixture.omegasllib")
add_test(
    NAME omegasl_archive_fixture_setup
    COMMAND $<TARGET_FILE:omegaslc>
            -t ${OMEGASL_TEST_TEMP_DIR}/archive_roundtrip_temp
            -o ${ARCHIVE_FIXTURE_LIB}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders.omegasl)
set_tests_properties(omegasl_archive_fixture_setup PROPERTIES FIXTURES_SETUP ARCHIVE_FIXTURE)

add_test(NAME omegasl_archive_roundtrip COMMAND ArchiveRoundTripTest ${ARCHIVE_FIXTURE_LIB})
set_tests_properties(omegasl_archive_roundtrip PROPERTIES FIXTURES_REQUIRED ARCHIVE_FIXTURE)

# =============================================================
# 4b. Preprocessor source-map host test (LSP Phase 5.1)
# =============================================================

# Host-only test: compiles the Preprocessor (+ its Lexer dependency) directly
# and asserts the output-line -> editor-line source map is correct once an
# `#include` expands inline — the foundation the language server relies on to
# keep diagnostics aligned with the buffer. Writes its own header fixture into
# the temp dir, so it needs no external `-I` or golden file.
add_executable(PreprocessorSourceMapTest
    preprocessor_sourcemap_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Preprocessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Lexer.cpp)
target_include_directories(PreprocessorSourceMapTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include)
target_compile_definitions(PreprocessorSourceMapTest PRIVATE ${PUBLIC_DEFS})
target_link_libraries(PreprocessorSourceMapTest PRIVATE OmegaCommonCore)

add_test(NAME omegasl_preprocessor_sourcemap
    COMMAND PreprocessorSourceMapTest ${OMEGASL_TEST_TEMP_DIR})

# --- End-to-end language-server #include resolution (LSP Phase 5.5) ---
# Drives omegasl-lsp over stdio with scripted JSON-RPC sessions: proves a
# relative `#include` resolves, an `-I` dir from a synthetic
# `omegasl_commands.json` resolves a sibling-dir header, header symbols feed
# completion but stay out of the document outline, and a malformed database is
# ignored (the server survives). Skipped when no python3 interpreter is found.
find_program(OMEGASL_PYTHON3 NAMES python3 python)
if(OMEGASL_PYTHON3)
    add_test(NAME omegasl_lsp_include_resolution
        COMMAND ${OMEGASL_PYTHON3}
                ${CMAKE_CURRENT_SOURCE_DIR}/lsp_include_test.py
                $<TARGET_FILE:omegasl-lsp>)
endif()

# =============================================================
# 5. `omegaslc --link` linker (Phase 2/3)
# =============================================================

# One fixture-setup step compiles two single-shader libs, `--link`s them into a
# merged lib, and writes a backend-bumped variant of one input. Every linker
# test then requires this fixture, so `ctest -R omegasl_link` regenerates its
# inputs first.
set(LINK_FIXTURE_DIR "${OMEGASL_TEST_TEMP_DIR}/link_fixtures")
add_test(
    NAME omegasl_link_fixture_setup
    COMMAND ${CMAKE_COMMAND}
            -DOMEGASLC=$<TARGET_FILE:omegaslc>
            -DARCHIVE_TOOL=$<TARGET_FILE:ArchiveRoundTripTest>
            -DSRC_A=${CMAKE_CURRENT_SOURCE_DIR}/link_a.omegasl
            -DSRC_B=${CMAKE_CURRENT_SOURCE_DIR}/link_b.omegasl
            -DTEMPDIR=${LINK_FIXTURE_DIR}/temp
            -DOUTDIR=${LINK_FIXTURE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/SetupLinkFixtures.cmake)
set_tests_properties(omegasl_link_fixture_setup PROPERTIES FIXTURES_SETUP LINK_FIXTURE)

# Positive: the merged lib holds Sigma(inputs) = 2 shaders and round-trips.
add_test(NAME omegasl_link_merge
    COMMAND ArchiveRoundTripTest ${LINK_FIXTURE_DIR}/link_merged.omegasllib 2)
set_tests_properties(omegasl_link_merge PROPERTIES FIXTURES_REQUIRED LINK_FIXTURE)

# Negative: a duplicate shader name across inputs is a hard error (link a lib
# with itself). One test guards the nonzero exit, one guards the diagnostic.
add_test(NAME omegasl_link_dup_fail
    COMMAND $<TARGET_FILE:omegaslc> --link
            ${LINK_FIXTURE_DIR}/link_a.omegasllib ${LINK_FIXTURE_DIR}/link_a.omegasllib
            -o ${LINK_FIXTURE_DIR}/link_dup.omegasllib)
set_tests_properties(omegasl_link_dup_fail PROPERTIES FIXTURES_REQUIRED LINK_FIXTURE WILL_FAIL TRUE)
add_test(NAME omegasl_link_dup_diag
    COMMAND $<TARGET_FILE:omegaslc> --link
            ${LINK_FIXTURE_DIR}/link_a.omegasllib ${LINK_FIXTURE_DIR}/link_a.omegasllib
            -o ${LINK_FIXTURE_DIR}/link_dup2.omegasllib)
set_tests_properties(omegasl_link_dup_diag PROPERTIES FIXTURES_REQUIRED LINK_FIXTURE
    PASS_REGULAR_EXPRESSION "duplicate shader name")

# Negative: inputs built for different backends cannot be merged (the payoff of
# the Phase 0 backend tag). Uses the backend-variant of link_a.
add_test(NAME omegasl_link_backend_fail
    COMMAND $<TARGET_FILE:omegaslc> --link
            ${LINK_FIXTURE_DIR}/link_a.omegasllib ${LINK_FIXTURE_DIR}/link_a_variant.omegasllib
            -o ${LINK_FIXTURE_DIR}/link_badbackend.omegasllib)
set_tests_properties(omegasl_link_backend_fail PROPERTIES FIXTURES_REQUIRED LINK_FIXTURE WILL_FAIL TRUE)
add_test(NAME omegasl_link_backend_diag
    COMMAND $<TARGET_FILE:omegaslc> --link
            ${LINK_FIXTURE_DIR}/link_a.omegasllib ${LINK_FIXTURE_DIR}/link_a_variant.omegasllib
            -o ${LINK_FIXTURE_DIR}/link_badbackend2.omegasllib)
set_tests_properties(omegasl_link_backend_diag PROPERTIES FIXTURES_REQUIRED LINK_FIXTURE
    PASS_REGULAR_EXPRESSION "backend mismatch")

# =============================================================
# 6. Golden-file tests for HLSL codegen (DirectX only)
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
