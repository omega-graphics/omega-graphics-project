include(CMakeParseArguments)

# The windowed Vulkan tests (2DTest, GPUTessTest, CPUTessTest, BlitTest) are now
# registered once, cross-platform, via add_gte_window_test in the parent
# gte/Tests/CMakeLists.txt (which owns the GTK-4 RunGTETestWindow object
# library). This file carries only the NON-windowed CLI / headless Vulkan
# tests. Those still link GTK 3 today — vestigial, but preserved until they are
# scrubbed of it — so we keep the gtk+-3.0 detection here.
pkg_check_modules(GTK gtk+-3.0)

# add_vulkan_cli_test builds a plain headless Vulkan test exe (no swap-chain
# window, no GTETestWindow object lib).
function(add_vulkan_cli_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;LIBS" ${ARGN})
    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    target_include_directories(${_ARG_NAME} PUBLIC ${GTK_INCLUDE_DIRS})
    target_link_directories(${_ARG_NAME} PRIVATE ${GTK_LIBRARY_DIRS})
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${GTK_LIBRARIES})
endfunction()

add_vulkan_cli_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.cpp")

# Extension 8 §8.6 — runtime sampler-state binding integration test (shared,
# backend-independent source under gte/tests/).
add_vulkan_cli_test(
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
add_vulkan_cli_test(
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
add_vulkan_cli_test(
    NAME
    "MeshShaderTest"
    SOURCES
    "../mesh_shader_test.cpp")
add_test(NAME omegagte_mesh_shader COMMAND MeshShaderTest)

# OmegaSL §16 Phase G — tessellated-draw integration test on the mandatory
# vertex-stage model (`vertex(tess=true)`). Exercises the full chain:
# OmegaSL → GLSL → shaderc → SPIR-V → VkGraphicsPipeline (vertex + tess-control
# + tess-evaluation + fragment stages) → startTessRenderPass / drawPatches →
# texture readback. Skips cleanly (exit 0) if the device does not advertise
# tessellation support. Backend-independent source; Metal/D3D12 can register it
# once their vertex-stage tessellation runtime lands.
add_vulkan_cli_test(
    NAME
    "TessellationVtxDrawTest"
    SOURCES
    "../tessellation_vtx_draw_test.cpp")
add_test(NAME omegagte_tessellation_vtx_draw COMMAND TessellationVtxDrawTest)

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
add_vulkan_cli_test(
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
add_vulkan_cli_test(
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

add_vulkan_cli_test(
    NAME
    "LibFileRoundTripTest"
    SOURCES
    "LibFileRoundTripTest/main.cpp")
add_dependencies(LibFileRoundTripTest roundtrip_lib_fixture)
target_compile_definitions(LibFileRoundTripTest PRIVATE "ROUNDTRIP_LIB_PATH=\"${RT_LIB}\"")
add_test(NAME omegagte_lib_file_roundtrip COMMAND LibFileRoundTripTest)
