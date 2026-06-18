include(CMakeParseArguments)

function(add_metal_test)
    cmake_parse_arguments("_ARG" "" "NAME;PLIST" "SOURCES;RESOURCES;FRAMEWORKS;LIBS" ${ARGN})
    get_target_property(OMEGA_GTE_LIB "OmegaGTE" RUNTIME_OUTPUT_DIRECTORY)
    set(OMEGA_GTE_LIB "${OMEGA_GTE_LIB}/OmegaGTE.framework")
    message("${OMEGA_GTE_LIB}")
    add_executable(${_ARG_NAME} MACOSX_BUNDLE ${_ARG_SOURCES} ${_ARG_RESOURCES})
    add_dependencies(${_ARG_NAME} "OmegaGTE")
    
    set(_FRAMEWORKS)
    foreach(f ${_ARG_FRAMEWORKS})
        find_library("__${f}" ${f})
        list(APPEND _FRAMEWORKS "${__${f}}")
    endforeach()

    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${_FRAMEWORKS})

    set_target_properties(${_ARG_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${_ARG_PLIST}
        RESOURCE "${_ARG_RESOURCES}"
        XCODE_EMBED_FRAMEWORKS "OmegaGTE"
        BUILD_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks"
    )
    target_link_options(${_ARG_NAME} PRIVATE
        "-Wl,-rpath,@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5"
        "-Wl,-rpath,@executable_path/../Frameworks"
    )

    # Embed OmegaGTE.framework and OmegaCommon so the app bundle is self-contained (Ninja/Unix make; Xcode uses XCODE_EMBED_FRAMEWORKS)
    set(_BUNDLE_CONTENTS "${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}.app/Contents")
    set(_BUNDLE_FRAMEWORKS "${_BUNDLE_CONTENTS}/Frameworks")
    add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_BUNDLE_FRAMEWORKS}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/Frameworks/OmegaGTE.framework" "${_BUNDLE_FRAMEWORKS}/OmegaGTE.framework"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/lib/libOmegaCommonCore.dylib" "${_BUNDLE_FRAMEWORKS}/libOmegaCommonCore.dylib"
        COMMENT "Embedding OmegaGTE.framework and OmegaCommonCore into ${_ARG_NAME}.app"
    )
endfunction()

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

add_omegasl_lib("2DTestShaders" "${CMAKE_CURRENT_SOURCE_DIR}/2DTest/shaders.omegasl" "${CMAKE_CURRENT_BINARY_DIR}/shaders.omegasllib")


add_metal_test(
    NAME 
    "2DTest"
    SOURCES
    "2DTest/main.mm"
    RESOURCES
    "2DTest/English.lproj/MainMenu.xib"
    PLIST 
    "${CMAKE_CURRENT_SOURCE_DIR}/2DTest/Info.plist"
    FRAMEWORKS 
    "Cocoa"
    "Metal"
    "QuartzCore"
)

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
    "GPUTessTest"
    SOURCES
    "GPUTessTest/main.mm"
    FRAMEWORKS
    "Metal"
    "QuartzCore"
    "AppKit"
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
