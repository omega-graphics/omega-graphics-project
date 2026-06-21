include(CMakeParseArguments)

# Cross-backend test-window runtime (GTETestWindow-CrossBackend-Plan.md, Phase
# 1). An OBJECT library so the Win32 windowing + run-loop implementation is
# compiled once and folded directly into each test exe — no extra link target
# for the tests to thread through. PUBLIC_DEFS (TARGET_DIRECTX / NOMINMAX) is
# inherited from the parent gte/CMakeLists.txt the same way the shared CLI
# tests pick it up; the include dirs mirror what OmegaGTE's headers need.
add_library(omegagte_testwindow_d3d12 OBJECT GTETestWindow_Win32.cpp)
target_include_directories(omegagte_testwindow_d3d12 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include")
target_compile_definitions(omegagte_testwindow_d3d12 PRIVATE ${PUBLIC_DEFS})

function(add_d3d12_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;LIBS;ASSETS;SHADERS" ${ARGN})

    add_executable(${_ARG_NAME} WIN32
        ${_ARG_SOURCES}
        $<TARGET_OBJECTS:omegagte_testwindow_d3d12>)
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS})

    add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${_ARG_NAME}> $<TARGET_FILE_DIR:${_ARG_NAME}>
        COMMAND_EXPAND_LISTS)

    # TARGET_RUNTIME_DLLS only fans out DLLs CMake resolves through the link
    # graph; it misses the third-party + sibling-module DLLs this project stages
    # into ${CMAKE_BINARY_DIR}/bin (OmegaCommon, ICU, OmegaGTE's own runtime).
    # omega_stage_runtime_dlls copies every *.dll from bin next to the test exe
    # so it actually launches. See cmake/OmegaGraphicsSuite.cmake.
    omega_stage_runtime_dlls(${_ARG_NAME})

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

# 2DTest now builds from the shared, backend-neutral body at
# gte/Tests/2DTest/main.cpp (Phase 1). The runtime assets (test.png, the
# OmegaSL source) still live under directx/2DTest/ for now — they relocate to
# gte/Tests/assets/2DTest/ in Phase 4, once Metal and Vulkan also consume them.
add_d3d12_test(
    NAME
    "2DTest"
    SOURCES
    "../2DTest/main.cpp"
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

# Mesh-Shader-Plan Phases 3 / 4b (and future raytracing) visual test.
# Phase 4b is live: the test loads `orange_tennis_racket.fbx` via
# `GEMeshAsset` and renders each triangle through a mesh-shader
# dispatch (one mesh threadgroup per triangle). Raytracing hooks join
# when that phase reaches the dispatch surface. The FBX asset lives
# under the shared `gte/tests/assets/` folder so other tests can reuse
# it; CMake stages it next to the test exe via the ASSETS list.
add_d3d12_test(
    NAME
    "MeshAndRaytracingTest"
    SOURCES
    "MeshAndRaytracingTest/main.cpp"
    ASSETS
    "../assets/orange_tennis_racket/orange_tennis_racket.fbx"
    SHADERS
    "MeshAndRaytracingTest/meshAndRaytracing.omegasl"
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

# OmegaSL §5.2 / §12 — matrix-op GPU integration test (shared, backend-
# independent source under gte/tests/).
add_d3d12_test(
    NAME
    "MatrixOpsTest"
    SOURCES
    "../matrix_ops_test.cpp"
)
add_test(NAME omegagte_matrix_ops COMMAND MatrixOpsTest)

# GPU-Commit-Timing plan (P1 — D3D12) — whole-commit GPU-timing integration test
# (shared, backend-independent source under gte/tests/). Exercises
# commitToGPUAndWaitTimed() (sync) and commitToGPU(handler) (async) against a
# compute commit and checks the reported GECommitCompletionInfo (status, buffer
# count, GPU duration). With P1 wired, the D3D12 timestamp-heap path now reports
# a real, non-zero GPU duration here on hardware that supports timestamp queries.
add_d3d12_test(
    NAME
    "CommitTimingTest"
    SOURCES
    "../commit_timing_test.cpp"
)
add_test(NAME omegagte_commit_timing COMMAND CommitTimingTest)
