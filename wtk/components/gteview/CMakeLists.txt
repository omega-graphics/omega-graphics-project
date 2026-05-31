# wtk/components/gteview/CMakeLists.txt
#
# OmegaWTK GTEView component — wraps OmegaGTE rendering into a Widget that
# embeds via NativeViewHost. See `wtk/docs/NativeViewHost-Adoption-Plan.md`
# Part 2 for the design and phasing (G1 → G4).
#
# This file is the BUILD WIRING ONLY. No implementation has landed yet;
# Phase G1 (and the §2.14 NativeVisualTree prerequisite it depends on) must
# be in before the source-file globs below actually find anything.

option(OMEGAWTK_COMPONENT_GTEVIEW "Build the GTEView component (3D viewport widget backed by OmegaGTE)" OFF)
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
# (MTLNativeContentNode etc.) so the core never has to depend on OmegaGTE.
if(TARGET_WIN32)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/win/*.cpp")
elseif(TARGET_MACOS)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/macos/*.mm")
elseif(TARGET_LINUX)
    file(GLOB GTEVIEW_NATIVE_SRCS "${GTEVIEW_SOURCE_DIR}/src/Native/gtk/*.cpp")
endif()
list(APPEND GTEVIEW_SRCS ${GTEVIEW_NATIVE_SRCS})

if(NOT GTEVIEW_SRCS)
    message(STATUS "OmegaWTKComponent_GTEView: no sources present yet (Phase G1 not landed); skipping.")
    return()
endif()

add_library(OmegaWTKComponent_GTEView STATIC ${GTEVIEW_SRCS})
set_target_properties(OmegaWTKComponent_GTEView PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(OmegaWTKComponent_GTEView
    PUBLIC  ${GTEVIEW_PUBLIC_INCLUDE_DIR}
            ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
            $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
    PRIVATE ${NATIVE_PRIVATE_INCLUDE_DIR})

# Core WTK target name varies by platform: framework on macOS, regular
# shared lib elsewhere. The framework name is exposed by
# `add_omega_graphics_module` as `<Name>.framework`.
if(TARGET_MACOS)
    add_dependencies(OmegaWTKComponent_GTEView OmegaWTK.framework)
    target_link_frameworks(OmegaWTKComponent_GTEView OmegaWTK OmegaGTE)
else()
    add_dependencies(OmegaWTKComponent_GTEView OmegaWTK)
    target_link_libraries(OmegaWTKComponent_GTEView PUBLIC OmegaWTK OmegaGTE)
endif()
target_link_libraries(OmegaWTKComponent_GTEView PUBLIC OmegaCommon)
