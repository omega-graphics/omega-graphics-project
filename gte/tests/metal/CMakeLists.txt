include(CMakeParseArguments)

# The windowed Metal tests (2DTest, GPUTessTest) are now registered once,
# cross-platform, via add_gte_window_test in the parent gte/Tests/CMakeLists.txt
# (which owns the Cocoa RunGTETestWindow object library and the bundle/
# framework-embedding logic). This file carries only the NON-windowed CLI /
# headless Metal tests, plus the Cocoa bundle template items (Info.plist /
# .nib) that the windowed tests still reference by path.

function(add_metal_cli_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;FRAMEWORKS;LIBS" ${ARGN})
    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    add_dependencies(${_ARG_NAME} "OmegaGTE")

    set(_FRAMEWORKS)
    foreach(f ${_ARG_FRAMEWORKS})
        find_library("__cli_${f}" ${f})
        list(APPEND _FRAMEWORKS "${__cli_${f}}")
    endforeach()

    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${_FRAMEWORKS})
    target_link_options(${_ARG_NAME} PRIVATE
        "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks/OmegaGTE.framework/Versions/0.5"
        "-Wl,-rpath,${CMAKE_BINARY_DIR}/lib"
    )
endfunction()

add_metal_cli_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.mm"
    FRAMEWORKS
    "Metal"
    "Foundation"
)

add_metal_cli_test(
    NAME
    "AssetTest"
    SOURCES
    "AssetTest/main.mm"
    FRAMEWORKS
    "Metal"
    "MetalKit"
    "ModelIO"
    "Foundation"
)
# Point the FBX case at one of ufbx's bundled test cubes (a plain 6-quad cube).
# Exercises the .fbx → shared MeshParser (ufbx) route, which Model I/O can't do.
target_compile_definitions("AssetTest" PRIVATE
    "OMEGA_ASSET_TEST_FBX=\"${CMAKE_CURRENT_SOURCE_DIR}/../../deps/ufbx/data/blender_272_cube_7400_binary.fbx\"")
# add_metal_cli_test only adds an rpath that points inside the framework
# (Versions/0.5); add the directory that *contains* the framework so the bare
# executable run from ctest / the shell can load OmegaGTE (same fix the matrix /
# bitfield / sampler CLI tests apply).
target_link_options("AssetTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")

# Extension 8 §8.6 — runtime sampler-state binding integration test (shared,
# backend-independent source under gte/tests/).
add_metal_cli_test(
    NAME
    "SamplerBindTest"
    SOURCES
    "../sampler_bind_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
# The framework's install name is `@rpath/OmegaGTE.framework/Versions/0.5/...`,
# so an rpath must point at the directory CONTAINING the framework for the bare
# executable (run by ctest, no app bundle) to load it. add_metal_cli_test only
# adds an rpath that points inside the framework, which works for the embedded
# bundle case but not here.
target_link_options("SamplerBindTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")

add_test(NAME omegagte_sampler_bind COMMAND SamplerBindTest)
# The negative case relies on a debug-build assertion firing; only register it
# (as a WILL_FAIL test) where asserts are active.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_test(NAME omegagte_sampler_bind_negative COMMAND SamplerBindTest negative)
    set_tests_properties(omegagte_sampler_bind_negative PROPERTIES WILL_FAIL TRUE)
endif()

# OmegaSL §5.2 / §12 — matrix-op GPU integration test (shared, backend-
# independent source under gte/tests/).
add_metal_cli_test(
    NAME
    "MatrixOpsTest"
    SOURCES
    "../matrix_ops_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("MatrixOpsTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_matrix_ops COMMAND MatrixOpsTest)

# OmegaSL §16 Phase E — tessellation hull-dispatch GPU integration test (shared,
# backend-independent source under gte/tests/). Metal-only today: only Metal
# models a `hull` as a compute dispatch with an intermediate factor buffer (on
# D3D12/Vulkan tessellation runs inside the draw pipeline, verified via
# drawPatches in the D3D12/Vulkan pass). Dispatches triHull + quadHull over one
# patch each and reads back the per-control-point output + the tessellation-
# factor half bits (1.0 == 0x3C00 tri, 2.0 == 0x4000 quad).
add_metal_cli_test(
    NAME
    "TessellationDispatchTest"
    SOURCES
    "../tessellation_dispatch_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("TessellationDispatchTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_tessellation_dispatch COMMAND TessellationDispatchTest)

# OmegaSL §5.3 — integer / bitfield-op GPU integration test (shared, backend-
# independent source under gte/tests/). Proves the firstbit normalization
# (countbits / reversebits / firstbithigh / firstbitlow) agrees with the host
# contract on real hardware.
add_metal_cli_test(
    NAME
    "BitfieldOpsTest"
    SOURCES
    "../bitfield_ops_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("BitfieldOpsTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_bitfield_ops COMMAND BitfieldOpsTest)

# OmegaSL §5.6 — atomic-operation GPU integration test (shared, backend-
# independent source under gte/tests/). Proves atomicity on real hardware
# (256 racing atomic_add -> exactly 256) plus load/store/exchange/and/min/max/or
# and the signed-atomic path.
add_metal_cli_test(
    NAME
    "AtomicOpsTest"
    SOURCES
    "../atomic_ops_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("AtomicOpsTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_atomic_ops COMMAND AtomicOpsTest)

# OmegaSL §5.5 Phase B — half-float pack / unpack GPU integration test (shared,
# backend-independent source under gte/tests/). Proves f16tof32 / f32tof16 /
# packHalf2x16 / unpackHalf2x16 round-trip a binary16 value bit-identically
# on real hardware across the three backend lowerings (HLSL native scalar +
# manual packed via statement injection, MSL as_type<half2>, GLSL native
# packed + scalar via the packed forms).
add_metal_cli_test(
    NAME
    "PackOpsTest"
    SOURCES
    "../pack_ops_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("PackOpsTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_pack_ops COMMAND PackOpsTest)

# OmegaSL §5.3 follow-up — integer / unsigned scalar+vector buffer-IO round-trip
# (shared, backend-independent source under gte/tests/). Readback counterpart to
# the writer's writeInt*/writeUint*; exercises the new GEBufferReader getInt*/
# getUint* scalar+vector API and the int/uint align-then-place padding.
add_metal_cli_test(
    NAME
    "IntVectorIOTest"
    SOURCES
    "../int_vector_io_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("IntVectorIOTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_int_vector_io COMMAND IntVectorIOTest)

# Mesh-Shader-Plan Phase 4c.4 — backend-independent mesh-shader integration
# test (shared, backend-independent source under gte/tests/). Exercises the
# full chain: OmegaSL → MSL → `metal` toolchain → `.metallib` →
# `MTLMeshRenderPipelineDescriptor` → `drawMeshThreadgroups` → fragment →
# texture → CPU readback. Lights up MSL emission from Phase 2c through the
# `metal` toolchain for the first time on this host.
add_metal_cli_test(
    NAME
    "MeshShaderTest"
    SOURCES
    "../mesh_shader_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("MeshShaderTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_mesh_shader COMMAND MeshShaderTest)

# Pipeline-Completion §2.2 / OmegaSL §10.2 — push-constant (`constant<T>`) GPU
# integration test (shared, backend-independent source under gte/tests/).
# Exercises setComputeConstants end-to-end: the command buffer scans the bound
# pipeline's layout for the push-constant slot and feeds the bytes to Metal
# `setBytes`.
add_metal_cli_test(
    NAME
    "PushConstantTest"
    SOURCES
    "../push_constant_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("PushConstantTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_push_constant COMMAND PushConstantTest)

# GPU-Commit-Timing plan (P2 + Metal) — whole-commit GPU-timing integration test
# (shared, backend-independent source under gte/tests/). Exercises the new
# commitToGPUAndWaitTimed() (sync) and commitToGPU(handler) (async) API against a
# compute commit and checks the reported GECommitCompletionInfo (status, buffer
# count, GPU duration) on real hardware.
add_metal_cli_test(
    NAME
    "CommitTimingTest"
    SOURCES
    "../commit_timing_test.cpp"
    FRAMEWORKS
    "Metal"
    "Foundation"
)
target_link_options("CommitTimingTest" PRIVATE
    "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks")
add_test(NAME omegagte_commit_timing COMMAND CommitTimingTest)
