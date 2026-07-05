include(CMakeParseArguments)

# The windowed D3D12 tests (2DTest, GPUTessTest, MeshAndRaytracingTest) are now
# registered once, cross-platform, via add_gte_window_test in the parent
# gte/Tests/CMakeLists.txt. This file carries only the NON-windowed CLI /
# headless D3D12 tests.
#
# add_d3d12_cli_test builds a headless D3D12 test exe: like the windowed path
# it is a WIN32 target linked through WinMainCRTStartup (GTE_TEST_ENTRY_POINT
# expands to WinMain on D3D12) with the runtime DLLs staged next to it, but it
# folds in no windowing object library and stages no assets/shaders — a CLI
# test opens no swap chain.
function(add_d3d12_cli_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;LIBS" ${ARGN})

    add_executable(${_ARG_NAME} WIN32 ${_ARG_SOURCES})
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

    target_link_options(${_ARG_NAME} PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup /MANIFEST:NO)
endfunction()

add_d3d12_cli_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.cpp"
)

# Extension 8 §8.6 — runtime sampler-state binding integration test (shared,
# backend-independent source under gte/tests/).
add_d3d12_cli_test(
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
add_d3d12_cli_test(
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
add_d3d12_cli_test(
    NAME
    "CommitTimingTest"
    SOURCES
    "../commit_timing_test.cpp"
)
add_test(NAME omegagte_commit_timing COMMAND CommitTimingTest)
