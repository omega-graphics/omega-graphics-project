# wtk/components/gteview/CMakeLists.txt
#
# OmegaWTK_GTEView — 3D viewport widget backed by OmegaGTE. Built as a
# SHARED library that ships as part of the OmegaWTK SDK: embedded inside
# `OmegaWTK.framework` on macOS, sibling of `OmegaWTK.dll`/`libOmegaWTK.so`
# on Windows/Linux. See `wtk/docs/NativeViewHost-Adoption-Plan.md` Part 2
# for the design and phasing (G1 → G4).
#
# `gteview` is the only INTERNAL component (lives in this repo because GTE's
# own tests exercise it). Every other component is an external repo fetched
# via AUTOMDEPS into a sibling `wtk/components/<name>/` directory — see
# `wtk/components/README.md` for the contract.
#
# This file is the BUILD WIRING ONLY. No implementation has landed yet;
# Phase G1 (and the §2.14 NativeVisualTree prerequisite it depends on) must
# be in before the source-file globs below actually find anything.

option(OMEGAWTK_COMPONENT_GTEVIEW "Build OmegaWTK_GTEView (3D viewport via OmegaGTE)" OFF)
if(NOT OMEGAWTK_COMPONENT_GTEVIEW)
    return()
endif()

set(GTEVIEW_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(GTEVIEW_PUBLIC_INCLUDE_DIR "${GTEVIEW_SOURCE_DIR}/include")

# Cross-platform sources.
file(GLOB GTEVIEW_SRCS "${GTEVIEW_SOURCE_DIR}/src/*.cpp")

# Per-platform GPU content node + visual-tree glue. Mirrors the split in
# `wtk/src/Native/{win,macos,gtk}/` — the component owns its own GPU
# variants (MTLGPUContentNode etc.) parallel to the core's video variants
# (MTLNativeContentNode etc.) so the core never depends on OmegaGTE.
if(TARGET_WIN32)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/win/*.cpp")
elseif(TARGET_MACOS)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/macos/*.mm")
elseif(TARGET_LINUX)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/gtk/*.cpp")
endif()
list(APPEND GTEVIEW_SRCS ${GTEVIEW_NATIVE_SRCS})

if(NOT GTEVIEW_SRCS)
    message(STATUS "OmegaWTK_GTEView: no sources present yet (Phase G1 not landed); skipping target creation.")
    return()
endif()

add_library(OmegaWTK_GTEView SHARED ${GTEVIEW_SRCS})

target_include_directories(OmegaWTK_GTEView
    PUBLIC  ${GTEVIEW_PUBLIC_INCLUDE_DIR}
            ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
            $<TARGET_PROPERTY:OmegaCommonCore,INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
    PRIVATE ${NATIVE_PRIVATE_INCLUDE_DIR})

# The component is a CONSUMER of OmegaWTK's exported types — flip OMEGAWTK_EXPORT
# from dllexport (its default) to dllimport so its TU's reference the core lib's
# symbols correctly on Win32. Mirrors what OmegaWTKApp does for an app build.
if(TARGET_WIN32)
    target_compile_definitions(OmegaWTK_GTEView PRIVATE OMEGAWTK_APP TARGET_WIN32 TARGET_DIRECTX)
elseif(TARGET_MACOS)
    target_compile_definitions(OmegaWTK_GTEView PRIVATE TARGET_MACOS TARGET_METAL)
elseif(TARGET_LINUX)
    target_compile_definitions(OmegaWTK_GTEView PRIVATE TARGET_GTK TARGET_VULKAN)
endif()

# When the component grows its own exported symbols (Phase G1+), define
# OMEGAWTK_GTEVIEW_BUILDING here and ship a matching Export.h header under
# include/omegaWTK/Components/GTEView/ mirroring wtk/include/OmegaWTKExport.h.
# Skipped for now — no exported symbols yet.

if(TARGET_MACOS)
    # Embed inside the OmegaWTK.framework bundle as a sibling shared lib.
    # The framework's EMBEDDED_LIBS path (in `add_framework_bundle`) copies
    # each entry into `OmegaWTK.framework/Versions/<V>/Libraries/` — the
    # rpath that OmegaWTKApp already sets on apps
    # (`@loader_path/../Frameworks/OmegaWTK.framework/Libraries`) resolves
    # them at run time. The actual EMBEDDED_LIBS argument is appended by
    # the parent wtk/CMakeLists.txt at framework-create time; this file
    # just builds the dylib and registers the target name.
else()
    # Win32 / Linux: ship as a sibling of libOmegaWTK in the standard
    # runtime output dir so omega_stage_runtime_dlls (Win32) and the
    # $ORIGIN rpath on the executable (Linux) find it without extra glue.
    if(WIN32)
        set_target_properties(OmegaWTK_GTEView PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    else()
        set_target_properties(OmegaWTK_GTEView PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    endif()
endif()

# Component lib needs PIC even when SHARED — OmegaGTE link + per-platform
# Native sublibs assume it.
set_target_properties(OmegaWTK_GTEView PROPERTIES POSITION_INDEPENDENT_CODE ON)

# OmegaGTE is always a direct dep — it's the whole point of gteview.
# The framework-link (OmegaWTK as a binary dependency) is added by the
# parent wtk/CMakeLists.txt after the framework target exists.
if(TARGET_MACOS)
    add_dependencies(OmegaWTK_GTEView OmegaGTE.framework)
    target_link_frameworks(OmegaWTK_GTEView OmegaGTE)
else()
    target_link_libraries(OmegaWTK_GTEView PUBLIC OmegaGTE OmegaCommonCore)
endif()

# Register this component for the framework's EMBEDDED_LIBS list (macOS) and
# for OmegaWTKApp's auto-link sweep (all platforms). The parent
# wtk/CMakeLists.txt reads this GLOBAL property in two places:
#   1. Before creating the framework, to pass component targets into
#      add_omega_graphics_module's EMBEDDED_LIBS argument.
#   2. After creating the framework, to call target_link_libraries on each
#      component so it picks up the OmegaWTK binary as its symbol source.
set_property(GLOBAL APPEND PROPERTY OMEGAWTK_COMPONENT_TARGETS OmegaWTK_GTEView)
