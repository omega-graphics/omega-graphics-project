include(CMakeParseArguments)

pkg_check_modules(GTK gtk+-3.0)

function(add_vulkan_test)
    cmake_parse_arguments("_ARG" "" "NAME" ";SOURCES;LIBS" ${ARGN})
    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    target_include_directories(${_ARG_NAME} PUBLIC ${GTK_INCLUDE_DIRS})
    target_link_directories(${_ARG_NAME} PRIVATE ${GTK_LIBRARY_DIRS})
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${GTK_LIBRARIES})
endfunction()

add_vulkan_test(
    NAME
    "2DTest"
    SOURCES 
    "2DTest/main.cpp")

add_vulkan_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.cpp")

add_vulkan_test(
    NAME
    "GPUTessTest"
    SOURCES
    "GPUTessTest/main.cpp")

add_vulkan_test(
    NAME
    "CPUTessTest"
    SOURCES
    "CPUTessTest/main.cpp")

add_vulkan_test(
    NAME
    "BlitTest"
    SOURCES
    "BlitTest/main.cpp")

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
target_link_libraries(VulkanQueueFamiliesTest PRIVATE OmegaCommon)
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
