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
