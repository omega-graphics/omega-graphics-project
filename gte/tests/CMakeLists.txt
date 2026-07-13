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

# Triangulation-Engine-Completion-Plan Phase 3.1 — backend-independent unit
# test for TEMesh::buildIndexed() vertex dedup. Pure CPU: only touches
# already-populated vertexPolygons, no GPU device needed.
add_executable(omegagte_te_index_buffer_test te_index_buffer_test.cpp)
target_include_directories(omegagte_te_index_buffer_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_compile_definitions(omegagte_te_index_buffer_test PRIVATE ${PUBLIC_DEFS})
target_link_libraries(omegagte_te_index_buffer_test PRIVATE OmegaCommonCore OmegaGTE)
add_test(NAME omegagte_te_index_buffer COMMAND omegagte_te_index_buffer_test)

# Triangulation-Engine-Completion-Plan Phase 9 — backend-independent unit test
# for TE's coordinate space: params-level viewport + precedence, viewport-origin
# awareness, the [0,1] depth map, and local-space (un-baked) triangulation.
# Pure CPU: OmegaTriangulationEngineContext is abstract but device-independent,
# so a test subclass gives a real triangulateSync with no GPU.
add_executable(omegagte_te_coordspace_test te_coordspace_test.cpp)
target_include_directories(omegagte_te_coordspace_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_compile_definitions(omegagte_te_coordspace_test PRIVATE ${PUBLIC_DEFS})
target_link_libraries(omegagte_te_coordspace_test PRIVATE OmegaCommonCore OmegaGTE)
add_test(NAME omegagte_te_coordspace COMMAND omegagte_te_coordspace_test)

# GESpace-Implementation-Plan Phase 1 — backend-independent unit test for the
# GESpace space->NDC matrix (origin-aware ortho, Y-flip, [0,1] depth) and for
# the column-major `transformPoint` it depends on. Pure CPU: GESpace is a
# transform authority, not a GPU resource, so no device is needed.
add_executable(omegagte_gespace_test gespace_test.cpp)
target_include_directories(omegagte_gespace_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_compile_definitions(omegagte_gespace_test PRIVATE ${PUBLIC_DEFS})
target_link_libraries(omegagte_gespace_test PRIVATE OmegaCommonCore OmegaGTE)
add_test(NAME omegagte_gespace COMMAND omegagte_gespace_test)

include(CMakeParseArguments)

# ===========================================================================
# Cross-backend windowed GTE tests (GTETestWindow-CrossBackend-Plan.md).
#
# Every windowed test (one that opens a swap-chain window through
# RunGTETestWindow) is declared ONCE here, cross-platform, via
# add_gte_window_test — there is no per-backend `add_d3d12_test` /
# `add_metal_test` / `add_vulkan_test` window entry anymore. The single
# function branches on the active TARGET_* internally and folds in the one
# `omegagte_testwindow` object library (whose source is the active backend's
# RunGTETestWindow implementation). The per-backend subdirectories below now
# carry only the NON-windowed CLI / headless tests (which have genuinely
# different per-backend link needs) plus the OS-specific bundle/manifest
# template files those windowed tests still reference by path.
# ===========================================================================

# --- Canonical per-test source + asset paths (one copy on disk each) -------
# Relative to this directory (gte/Tests), which is where add_gte_window_test is
# called from (so CMAKE_CURRENT_SOURCE_DIR inside it resolves these correctly).
set(GTE_TEST_2DTEST_SOURCES "2DTest/main.cpp")
set(GTE_TEST_2DTEST_ASSETS  "assets/2DTest/test.png")
set(GTE_TEST_2DTEST_SHADERS "assets/2DTest/shaders.omegasl")

set(GTE_TEST_GPUTESSTEST_SOURCES "GPUTessTest/main.cpp")

set(GTE_TEST_MESHANDRAYTRACINGTEST_SOURCES "MeshAndRaytracingTest/main.cpp")
set(GTE_TEST_MESHANDRAYTRACINGTEST_ASSETS  "assets/orange_tennis_racket/orange_tennis_racket.fbx")
set(GTE_TEST_MESHANDRAYTRACINGTEST_SHADERS "assets/MeshAndRaytracingTest/meshAndRaytracing.omegasl")

set(GTE_TEST_BLITTEST_SOURCES "BlitTest/main.cpp")
set(GTE_TEST_BLITTEST_SHADERS "assets/BlitTest/shaders.omegasl")

set(GTE_TEST_CPUTESSTEST_SOURCES "CPUTessTest/main.cpp")

# --- The one shared RunGTETestWindow object library ------------------------
# Source = the active backend's implementation. Folded into each windowed
# test exe via $<TARGET_OBJECTS:omegagte_testwindow>, so no extra link target
# the test exes have to thread through. _GTE_TESTWINDOW_OK gates registration
# so a host missing a hard dependency (e.g. Vulkan without GTK 4) skips the
# windowed tests cleanly instead of erroring.
set(_GTE_TESTWINDOW_OK FALSE)
if(TARGET_DIRECTX)
    add_library(omegagte_testwindow OBJECT directx/GTETestWindow_Win32.cpp)
    set(_GTE_TESTWINDOW_OK TRUE)
elseif(TARGET_METAL AND NOT IOS)
    add_library(omegagte_testwindow OBJECT metal/GTETestWindow_Cocoa.mm)
    set(_GTE_TESTWINDOW_OK TRUE)
elseif(TARGET_VULKAN)
    find_package(PkgConfig)
    # GTK 4 carries the bare-GdkSurface windowing the Vulkan backend needs.
    pkg_check_modules(GTK4 gtk4)
    if(GTK4_FOUND)
        add_library(omegagte_testwindow OBJECT vulkan/GTETestWindow_GTK.cpp)
        target_include_directories(omegagte_testwindow PRIVATE ${GTK4_INCLUDE_DIRS})
        set(_GTE_TESTWINDOW_OK TRUE)
    else()
        message(WARNING "OmegaGTE tests: gtk4 not found — skipping all windowed GTE tests.")
    endif()
endif()

if(_GTE_TESTWINDOW_OK)
    target_include_directories(omegagte_testwindow PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
    target_compile_definitions(omegagte_testwindow PRIVATE ${PUBLIC_DEFS})
endif()

# --- The single cross-platform windowed-test entry point -------------------
# add_gte_window_test(
#     NAME       <exe name>
#     SOURCES    <shared body>
#     [ASSETS    <files staged next to the exe / into the .app>]
#     [SHADERS   <.omegasl compiled to .omegasllib and staged>]
#     [LIBS      <extra link libs>]
#     [BACKENDS  <subset of: d3d12 metal vulkan; default = all>]
#     [PLIST     <Info.plist>]        # macOS bundle only; ignored elsewhere
#     [RESOURCES <bundle resources>]  # macOS bundle only; ignored elsewhere
#     [FRAMEWORKS <extra frameworks>] # macOS only; Cocoa/Metal/QuartzCore are
#                                     # always linked for the window impl
# )
function(add_gte_window_test)
    cmake_parse_arguments("_W" "" "NAME;PLIST"
        "SOURCES;ASSETS;SHADERS;LIBS;RESOURCES;FRAMEWORKS;BACKENDS" ${ARGN})

    if(NOT _GTE_TESTWINDOW_OK)
        return()
    endif()

    # Skip a test on any backend it does not support (BACKENDS lists the ones
    # it does; empty = every backend).
    if(_W_BACKENDS)
        set(_this_backend "")
        if(TARGET_DIRECTX)
            set(_this_backend "d3d12")
        elseif(TARGET_METAL)
            set(_this_backend "metal")
        elseif(TARGET_VULKAN)
            set(_this_backend "vulkan")
        endif()
        if(NOT _this_backend IN_LIST _W_BACKENDS)
            return()
        endif()
    endif()

    if(TARGET_DIRECTX)
        # WIN32 exe, WinMain entry (GTE_TEST_ENTRY_POINT), swap-chain window.
        add_executable(${_W_NAME} WIN32
            ${_W_SOURCES}
            $<TARGET_OBJECTS:omegagte_testwindow>)
        target_link_libraries(${_W_NAME} PRIVATE "OmegaGTE" ${_W_LIBS})
        # TARGET_RUNTIME_DLLS only fans out DLLs CMake resolves through the
        # link graph; omega_stage_runtime_dlls copies the third-party + sibling
        # DLLs staged into bin/ (OmegaCommon, ICU, OmegaGTE's runtime) so the
        # exe actually launches.
        add_custom_command(TARGET ${_W_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${_W_NAME}> $<TARGET_FILE_DIR:${_W_NAME}>
            COMMAND_EXPAND_LISTS)
        omega_stage_runtime_dlls(${_W_NAME})
        target_link_options(${_W_NAME} PRIVATE
            /SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup /MANIFEST:NO)
    elseif(TARGET_METAL)
        add_executable(${_W_NAME} MACOSX_BUNDLE
            ${_W_SOURCES}
            ${_W_RESOURCES}
            $<TARGET_OBJECTS:omegagte_testwindow>)
        add_dependencies(${_W_NAME} "OmegaGTE")
        # The Cocoa RunGTETestWindow impl always needs these three; a test may
        # append more via FRAMEWORKS.
        set(_fw_names Cocoa Metal QuartzCore ${_W_FRAMEWORKS})
        set(_fw_libs)
        foreach(f ${_fw_names})
            find_library("__gtewin_${f}" ${f})
            list(APPEND _fw_libs "${__gtewin_${f}}")
        endforeach()
        target_link_libraries(${_W_NAME} PRIVATE "OmegaGTE" ${_W_LIBS} ${_fw_libs})
        set_target_properties(${_W_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${_W_PLIST}"
            RESOURCE "${_W_RESOURCES}"
            XCODE_EMBED_FRAMEWORKS "OmegaGTE"
            BUILD_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks"
            INSTALL_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks")
        target_link_options(${_W_NAME} PRIVATE
            "-Wl,-rpath,@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5"
            "-Wl,-rpath,@executable_path/../Frameworks")
        # Embed OmegaGTE.framework + OmegaCommon so the bundle is self-contained
        # (Ninja/Unix Make; Xcode uses XCODE_EMBED_FRAMEWORKS above).
        set(_bundle_frameworks "${CMAKE_CURRENT_BINARY_DIR}/${_W_NAME}.app/Contents/Frameworks")
        add_custom_command(TARGET ${_W_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle_frameworks}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/Frameworks/OmegaGTE.framework" "${_bundle_frameworks}/OmegaGTE.framework"
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/lib/libOmegaCommonCore.dylib" "${_bundle_frameworks}/libOmegaCommonCore.dylib"
            COMMENT "Embedding OmegaGTE.framework and OmegaCommonCore into ${_W_NAME}.app")
    elseif(TARGET_VULKAN)
        # Plain exe. GTK 4 (linked here) resolves the object lib's GDK/GTK
        # symbols; the shared test body itself pulls no GTK.
        add_executable(${_W_NAME}
            ${_W_SOURCES}
            $<TARGET_OBJECTS:omegagte_testwindow>)
        target_include_directories(${_W_NAME} PUBLIC ${GTK4_INCLUDE_DIRS})
        target_link_directories(${_W_NAME} PRIVATE ${GTK4_LIBRARY_DIRS})
        target_link_libraries(${_W_NAME} PRIVATE "OmegaGTE" ${_W_LIBS} ${GTK4_LIBRARIES})
    endif()

    # --- Shared asset + shader staging (identical across all backends) ------
    # The test chdir's to getExecutableDir() at startup, so its "./<file>"
    # loads resolve next to the exe — $<TARGET_FILE_DIR> is the exe's dir on
    # every backend (Contents/MacOS for a .app bundle).
    foreach(_asset IN LISTS _W_ASSETS)
        if(NOT IS_ABSOLUTE "${_asset}")
            set(_asset_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}")
        else()
            set(_asset_abs "${_asset}")
        endif()
        get_filename_component(_asset_name "${_asset_abs}" NAME)
        add_custom_command(TARGET ${_W_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_asset_abs}" "$<TARGET_FILE_DIR:${_W_NAME}>/${_asset_name}")
    endforeach()

    # Compile each OmegaSL source to a .omegasllib and stage it next to the
    # exe. INCLUDE_DIRS points omegaslc at the shader's own directory so a
    # shared `#include "*.omegaslh"` there resolves.
    foreach(_shader IN LISTS _W_SHADERS)
        if(NOT IS_ABSOLUTE "${_shader}")
            set(_shader_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}")
        else()
            set(_shader_abs "${_shader}")
        endif()
        get_filename_component(_shader_basename "${_shader_abs}" NAME_WE)
        get_filename_component(_shader_dir "${_shader_abs}" DIRECTORY)
        set(_shader_target "${_W_NAME}_${_shader_basename}_Shader")
        set(_shader_output "${CMAKE_CURRENT_BINARY_DIR}/${_shader_basename}.omegasllib")
        add_omegasl_lib("${_shader_target}" "${_shader_abs}" "${_shader_output}"
            INCLUDE_DIRS "${_shader_dir}")
        add_dependencies(${_W_NAME} "${_shader_target}")
        add_custom_command(TARGET ${_W_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_shader_output}" "$<TARGET_FILE_DIR:${_W_NAME}>/${_shader_basename}.omegasllib")
    endforeach()
endfunction()

# --- Windowed-test registrations (one call each, all backends) -------------
# 2DTest / GPUTessTest support every backend (default BACKENDS). PLIST /
# RESOURCES are macOS-bundle template items — passed unconditionally and
# ignored on D3D12 / Vulkan.
add_gte_window_test(
    NAME       2DTest
    SOURCES    ${GTE_TEST_2DTEST_SOURCES}
    ASSETS     ${GTE_TEST_2DTEST_ASSETS}
    SHADERS    ${GTE_TEST_2DTEST_SHADERS}
    PLIST      "${CMAKE_CURRENT_SOURCE_DIR}/metal/2DTest/Info.plist"
    RESOURCES  "metal/2DTest/English.lproj/MainMenu.xib")

add_gte_window_test(
    NAME       GPUTessTest
    SOURCES    ${GTE_TEST_GPUTESSTEST_SOURCES}
    PLIST      "${CMAKE_CURRENT_SOURCE_DIR}/metal/GPUTessTest/Info.plist")

# D3D12-only today (mesh shaders have landed, but there is no amplifaction stage globally.)
add_gte_window_test(
    NAME       MeshAndRaytracingTest
    SOURCES    ${GTE_TEST_MESHANDRAYTRACINGTEST_SOURCES}
    ASSETS     ${GTE_TEST_MESHANDRAYTRACINGTEST_ASSETS}
    SHADERS    ${GTE_TEST_MESHANDRAYTRACINGTEST_SHADERS})

# Vulkan-only today (single-backend bodies; portable when other backends wire
# the equivalent passes).
# add_gte_window_test(
#     NAME       BlitTest
#     SOURCES    ${GTE_TEST_BLITTEST_SOURCES}
#     SHADERS    ${GTE_TEST_BLITTEST_SHADERS})

add_gte_window_test(
    NAME       CPUTessTest
    BACKENDS   vulkan
    SOURCES    ${GTE_TEST_CPUTESSTEST_SOURCES})

# --- Non-windowed (CLI / headless) tests, per backend ----------------------
# These have genuinely different per-backend link needs (D3D12 WinMain
# subsystem + DLL staging, Metal framework rpaths, Vulkan plain/GTK links), so
# each backend keeps its own small `add_<backend>_cli_test` helper for them.
if(TARGET_DIRECTX)
    add_subdirectory("directx")
elseif(TARGET_METAL AND NOT IOS)
    add_subdirectory("metal")
elseif(TARGET_VULKAN)
    add_subdirectory("vulkan")
endif()