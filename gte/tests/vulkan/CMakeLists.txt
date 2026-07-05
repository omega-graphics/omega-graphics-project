include(CMakeParseArguments)

pkg_check_modules(GTK gtk+-3.0)
# §2.15 GTK4 Migration: 2DTest is ported to GTK 4 ahead of the other windowing
# tests as the G4-3 "render Vulkan into the toplevel GdkSurface" probe. Detect
# gtk4 non-REQUIRED so hosts without it still configure (2DTest is then skipped);
# the remaining windowing tests stay on gtk+-3.0 until they are converted too.
pkg_check_modules(GTK4 gtk4)

# Cross-backend test-window runtime (GTETestWindow-CrossBackend-Plan.md, Phase
# 3). An OBJECT library so the GTK-4 bare-GdkSurface windowing + run-loop
# implementation is compiled once and folded directly into each windowed test
# exe — no extra link target for the tests to thread through. Built only when
# gtk4 is present (the windowed 2DTest is likewise gated on GTK4_FOUND below).
# PUBLIC_DEFS (TARGET_VULKAN + the VULKAN_TARGET_* backend macro) is inherited
# from the parent gte/CMakeLists.txt, so the descriptor fill here matches the
# same #ifdef gating GE.h uses for the NativeRenderTargetDescriptor fields.
if(GTK4_FOUND)
    add_library(omegagte_testwindow_vulkan OBJECT GTETestWindow_GTK.cpp)
    target_include_directories(omegagte_testwindow_vulkan PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include"
        ${GTK4_INCLUDE_DIRS})
    target_compile_definitions(omegagte_testwindow_vulkan PRIVATE ${PUBLIC_DEFS})
endif()

function(add_vulkan_test)
    cmake_parse_arguments("_ARG" "GTK4" "NAME" "SOURCES;LIBS;ASSETS;SHADERS" ${ARGN})
    if(_ARG_GTK4)
        # Windowed test: fold in the shared GTETestWindow object lib and link
        # GTK 4 (which resolves the object lib's GDK/GTK symbols). The test body
        # itself is the platform-independent shared source and pulls no GTK.
        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            $<TARGET_OBJECTS:omegagte_testwindow_vulkan>)
        target_include_directories(${_ARG_NAME} PUBLIC ${GTK4_INCLUDE_DIRS})
        target_link_directories(${_ARG_NAME} PRIVATE ${GTK4_LIBRARY_DIRS})
        target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${GTK4_LIBRARIES})

        # Stage runtime assets next to the exe. The test chdir's to
        # getExecutableDir() at startup, so its "./<file>" loads resolve there;
        # for a plain executable $<TARGET_FILE_DIR> is CMAKE_CURRENT_BINARY_DIR.
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

        # Compile each OmegaSL source to a .omegasllib in the exe's output dir
        # (CMAKE_CURRENT_BINARY_DIR). INCLUDE_DIRS points omegaslc at the
        # shader's own directory so a shared `#include "*.omegaslh"` resolves.
        foreach(_shader IN LISTS _ARG_SHADERS)
            if(NOT IS_ABSOLUTE "${_shader}")
                set(_shader_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}")
            else()
                set(_shader_abs "${_shader}")
            endif()
            get_filename_component(_shader_basename "${_shader_abs}" NAME_WE)
            get_filename_component(_shader_dir "${_shader_abs}" DIRECTORY)
            set(_shader_target "${_ARG_NAME}_${_shader_basename}_Shader")
            set(_shader_output "${CMAKE_CURRENT_BINARY_DIR}/${_shader_basename}.omegasllib")
            add_omegasl_lib("${_shader_target}" "${_shader_abs}" "${_shader_output}"
                INCLUDE_DIRS "${_shader_dir}")
            add_dependencies(${_ARG_NAME} "${_shader_target}")
        endforeach()
    else()
        add_executable(${_ARG_NAME} ${_ARG_SOURCES})
        target_include_directories(${_ARG_NAME} PUBLIC ${GTK_INCLUDE_DIRS})
        target_link_directories(${_ARG_NAME} PRIVATE ${GTK_LIBRARY_DIRS})
        target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${GTK_LIBRARIES})
    endif()
endfunction()

# 2DTest now builds from the shared, backend-neutral body declared once in
# gte/Tests/CMakeLists.txt (GTE_TEST_2DTEST_*, GTETestWindow-CrossBackend-
# Plan.md Phase 3). The GTK-4 windowing / run-loop lives behind the
# omegagte_testwindow_vulkan object lib; the runtime assets (test.png, the
# OmegaSL source) are the canonical shared copies under
# gte/tests/assets/2DTest/ — the shader is precompiled to a .omegasllib here
# (Open Decision #2) and test.png is loaded portably through GETextureAsset
# (OmegaCommon::Img on Vulkan).
if(GTK4_FOUND)
    add_vulkan_test(
        GTK4
        NAME
        "2DTest"
        SOURCES
        ${GTE_TEST_2DTEST_SOURCES}
        ASSETS
        ${GTE_TEST_2DTEST_ASSETS}
        SHADERS
        ${GTE_TEST_2DTEST_SHADERS})
else()
    message(WARNING "OmegaGTE tests: gtk4 not found — skipping the GTK-4-ported 2DTest.")
endif()

add_vulkan_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.cpp")

# GPUTessTest / CPUTessTest / BlitTest now build from the shared, backend-
# neutral bodies declared once in gte/Tests/CMakeLists.txt
# (GTE_TEST_GPUTESSTEST_* / GTE_TEST_CPUTESSTEST_* / GTE_TEST_BLITTEST_*,
# Phase 4), through the same GTK-4 windowed path 2DTest uses.
# GPUTessTest/CPUTessTest render nothing — they only need a
# NativeRenderTargetDescriptor to build a TE context from — so they compute
# their comparison in onReady and self-close with the pass/fail exit code via
# RequestGTETestWindowClose.
if(GTK4_FOUND)
    add_vulkan_test(
        GTK4
        NAME
        "GPUTessTest"
        SOURCES
        ${GTE_TEST_GPUTESSTEST_SOURCES})

    add_vulkan_test(
        GTK4
        NAME
        "CPUTessTest"
        SOURCES
        ${GTE_TEST_CPUTESSTEST_SOURCES})

    add_vulkan_test(
        GTK4
        NAME
        "BlitTest"
        SOURCES
        ${GTE_TEST_BLITTEST_SOURCES}
        SHADERS
        ${GTE_TEST_BLITTEST_SHADERS})
else()
    message(WARNING "OmegaGTE tests: gtk4 not found — skipping GPUTessTest/CPUTessTest/BlitTest (GTK-4 windowed).")
endif()

# Extension 8 §8.6 — runtime sampler-state binding integration test (shared,
# backend-independent source under gte/tests/).
add_vulkan_test(
    NAME
    "SamplerBindTest"
    SOURCES
    "../sampler_bind_test.cpp")
add_test(NAME omegagte_sampler_bind COMMAND SamplerBindTest)
# The negative case relies on a debug-build assertion firing; only register it
# (as a WILL_FAIL test) where asserts are active.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_test(NAME omegagte_sampler_bind_negative COMMAND SamplerBindTest negative)
    set_tests_properties(omegagte_sampler_bind_negative PROPERTIES WILL_FAIL TRUE)
endif()

# OmegaSL §5.2 / §12 — matrix-op GPU integration test (shared, backend-
# independent source under gte/tests/).
add_vulkan_test(
    NAME
    "MatrixOpsTest"
    SOURCES
    "../matrix_ops_test.cpp")
add_test(NAME omegagte_matrix_ops COMMAND MatrixOpsTest)

# Mesh-Shader-Plan Phase 4a verification — backend-independent mesh-shader
# integration test (shared, backend-independent source under gte/tests/).
# Exercises the full chain: OmegaSL → GLSL → glslc → SPIR-V →
# VkGraphicsPipeline (mesh+fragment stages) → vkCmdDrawMeshTasksEXT →
# texture readback. Skips cleanly if the device does not advertise mesh
# shader support — `omegagte_mesh_shader` exits 0 with a SKIP line in
# that case so it doesn't false-fail on hardware without the feature.
add_vulkan_test(
    NAME
    "MeshShaderTest"
    SOURCES
    "../mesh_shader_test.cpp")
add_test(NAME omegagte_mesh_shader COMMAND MeshShaderTest)

# CommandQueue-Typed-Pool Phase 2 — pure-CPU unit test for the queue-family
# picker. Pulls in the in-tree VulkanQueueFamilies translation unit
# directly so no GPU device is required. Deterministic and fast.
add_executable(VulkanQueueFamiliesTest
    "VulkanQueueFamiliesTest/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vulkan/VulkanQueueFamilies.cpp")
target_include_directories(VulkanQueueFamiliesTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include")
target_compile_definitions(VulkanQueueFamiliesTest PRIVATE ${PUBLIC_DEFS})
target_link_libraries(VulkanQueueFamiliesTest PRIVATE OmegaCommonCore)
add_test(NAME omegagte_vulkan_queue_families COMMAND VulkanQueueFamiliesTest)

# CommandQueue-Typed-Pool Phase 2 — backend-independent integration test
# that creates one queue of each `GECommandQueueDesc::Type` against the
# real GTE device, asserts type()/isDedicated()/priority()/label() round-
# trip, and verifies makeNativeRenderTarget rejects a Transfer present
# queue. Source is shared so D3D12 / Metal CMakeLists can wire it the
# same way later.
add_vulkan_test(
    NAME
    "CommandQueueDescTest"
    SOURCES
    "../command_queue_desc_test.cpp")
add_test(NAME omegagte_command_queue_desc COMMAND CommandQueueDescTest)

# GPU-Commit-Timing plan (P1 — Vulkan) — whole-commit GPU-timing integration
# test (shared, backend-independent source under gte/tests/). Exercises the
# commitToGPUAndWaitTimed() (sync) and commitToGPU(handler) (async) API against a
# compute commit and checks the reported GECommitCompletionInfo (status, buffer
# count, GPU duration) on real hardware. With the Vulkan per-buffer timestamp
# writes in place it reports real GPU times on a device that advertises
# GTEDEVICE_FEATURE_TIMESTAMP_QUERIES.
add_vulkan_test(
    NAME
    "CommitTimingTest"
    SOURCES
    "../commit_timing_test.cpp")
add_test(NAME omegagte_commit_timing COMMAND CommitTimingTest)

# OmegaSL linker Phase 0 — `.omegasllib` on-disk container prefix round-trip.
# omegaslc compiles a fixture shader into a real `.omegasllib` (with the
# magic / version / backend-id prefix) at build time; the test loads it back
# through loadShaderLibrary to verify the writer↔reader lockstep, plus that a
# corrupted-magic and an unknown-version copy are both rejected. This is the
# only test that exercises the file (not runtime) library path on Vulkan.
set(RT_LIB "${CMAKE_CURRENT_BINARY_DIR}/roundtrip.omegasllib")
add_custom_command(
    OUTPUT "${RT_LIB}"
    COMMAND $<TARGET_FILE:omegaslc>
            -t "${CMAKE_CURRENT_BINARY_DIR}/roundtrip_temp"
            -o "${RT_LIB}"
            "${CMAKE_CURRENT_SOURCE_DIR}/LibFileRoundTripTest/roundtrip.omegasl"
    DEPENDS omegaslc "${CMAKE_CURRENT_SOURCE_DIR}/LibFileRoundTripTest/roundtrip.omegasl"
    COMMENT "omegaslc: compiling roundtrip.omegasllib fixture")
add_custom_target(roundtrip_lib_fixture DEPENDS "${RT_LIB}")

add_vulkan_test(
    NAME
    "LibFileRoundTripTest"
    SOURCES
    "LibFileRoundTripTest/main.cpp")
add_dependencies(LibFileRoundTripTest roundtrip_lib_fixture)
target_compile_definitions(LibFileRoundTripTest PRIVATE "ROUNDTRIP_LIB_PATH=\"${RT_LIB}\"")
add_test(NAME omegagte_lib_file_roundtrip COMMAND LibFileRoundTripTest)
