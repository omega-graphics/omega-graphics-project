enable_testing()

# Backend-independent unit test for the std430 / std140 buffer-layout math
# (OmegaSL §2.4). Pure CPU — no GPU device needed, so it runs on any host
# (including Metal builds, where omegaSLStructStride itself never takes the
# std140 path). Exercises the BufferIO.h helpers directly.
add_executable(omegagte_std140_layout_test std140_layout_test.cpp)
target_include_directories(omegagte_std140_layout_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(omegagte_std140_layout_test PRIVATE OmegaCommonCore)
add_test(NAME omegagte_std140_layout COMMAND omegagte_std140_layout_test)

# Extension 8 §8.5/§8.6 — backend-independent unit test for the runtime
# sampler-bind validation guard (`validateSamplerBindKind`). Pure CPU, but it
# includes the public GTE headers, which require the active target macro
# (PUBLIC_DEFS, set by the parent gte/CMakeLists.txt) to compile.
add_executable(omegagte_sampler_validation_test sampler_validation_test.cpp)
target_include_directories(omegagte_sampler_validation_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_compile_definitions(omegagte_sampler_validation_test PRIVATE ${PUBLIC_DEFS})
target_link_libraries(omegagte_sampler_validation_test PRIVATE OmegaCommonCore OmegaGTE)
add_test(NAME omegagte_sampler_validation COMMAND omegagte_sampler_validation_test)

# Shared GUI test registry (GTETestWindow-CrossBackend-Plan.md). Each migrated
# test's shared source + runtime assets are declared exactly ONCE here; the
# per-backend CMakeLists.txt (directx/ metal/ vulkan/) only reference these
# variables in their add_d3d12_test/add_metal_test/add_vulkan_test calls —
# they never re-type a source/asset/shader path themselves. Adding a backend
# to an existing test, or relocating a shared asset, touches this list only.
set(GTE_TEST_2DTEST_SOURCES "../2DTest/main.cpp")
set(GTE_TEST_2DTEST_ASSETS  "../assets/2DTest/test.png")
set(GTE_TEST_2DTEST_SHADERS "../assets/2DTest/shaders.omegasl")

set(GTE_TEST_GPUTESSTEST_SOURCES "../GPUTessTest/main.cpp")

set(GTE_TEST_MESHANDRAYTRACINGTEST_SOURCES "../MeshAndRaytracingTest/main.cpp")
set(GTE_TEST_MESHANDRAYTRACINGTEST_ASSETS  "../assets/orange_tennis_racket/orange_tennis_racket.fbx")
set(GTE_TEST_MESHANDRAYTRACINGTEST_SHADERS "../assets/MeshAndRaytracingTest/meshAndRaytracing.omegasl")

set(GTE_TEST_BLITTEST_SOURCES "../BlitTest/main.cpp")
set(GTE_TEST_BLITTEST_SHADERS "../assets/BlitTest/shaders.omegasl")

set(GTE_TEST_CPUTESSTEST_SOURCES "../CPUTessTest/main.cpp")

if(TARGET_DIRECTX)
    add_subdirectory("directx")
elseif(TARGET_METAL AND NOT IOS)
    add_subdirectory("metal")
elseif(TARGET_VULKAN)
    add_subdirectory("vulkan")
endif()