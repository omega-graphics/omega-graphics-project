add_subdirectory("RootWidget")
# # if(SWIFT_BUILD)
# #     add_subdirectory("SwiftImport")
# # endif()

add_subdirectory(MediaCodecTest)

OmegaWTKApp(
    NAME
    BasicAppTest
    BUNDLE_ID
    "org.omegagraphics.BasicAppTest"
    SOURCES
    BasicAppTest/main.cpp
    BasicAppTest/BasicAppTestRun.cpp)
target_compile_options(BasicAppTest PRIVATE -g -O0)

OmegaWTKApp(
    NAME
    EllipsePathCompositorTest
    BUNDLE_ID
    "org.omegagraphics.EllipsePathCompositorTest"
    SOURCES
    EllipsePathCompositorTest/main.cpp)

OmegaWTKApp(
    NAME
    TextCompositorTest
    BUNDLE_ID
    "org.omegagraphics.TextCompositorTest"
    SOURCES
    TextCompositorTest/main.cpp)

OmegaWTKApp(
    NAME
    ImageRenderTest
    BUNDLE_ID
    "org.omegagraphics.ImageRenderTest"
    SOURCES
    ImageRenderTest/main.cpp)
# Stage the test PNG next to the built binary so the test loads it via
# the relative `"test.png"` fallback in main.cpp regardless of which
# source tree (or none) is reachable from the run host. The previous
# CMake compile-def baked an absolute source-tree path into the EXE,
# which breaks Linux + Windows CI runs that build out-of-tree or ship
# the binary somewhere the source doesn't follow. copy_if_different
# avoids re-stamping the build timestamp on no-op rebuilds.
add_custom_command(TARGET ImageRenderTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ImageRenderTest/test.png"
        "$<TARGET_FILE_DIR:ImageRenderTest>/test.png"
    COMMENT "Staging ImageRenderTest/test.png next to ImageRenderTest binary")

OmegaWTKApp(
    NAME
    ContainerClampAnimationTest
    BUNDLE_ID
    "org.omegagraphics.ContainerClampAnimationTest"
    SOURCES
    ContainerClampAnimationTest/main.cpp)

# Phase-1 text-layout-engine unit test (Text-Layout-Engine-Plan.md).
# Plain executable — no GTK / DX / Cocoa main, so it runs headless on
# CI as well as locally. Links the composition static lib for the
# layout engine itself + OmegaWTK_Core for the `OmegaWTK::gte` global
# that `GlyphAtlas` (transitively pulled in by `Font`'s ctor) refers
# to. The test never touches `gte` at runtime — it just needs to
# satisfy the linker.
add_executable(TextLayoutEngineTest TextLayoutEngineTest/main.cpp)
target_link_libraries(TextLayoutEngineTest PRIVATE
    OmegaWTK_Composition
    OmegaWTK_Core
    OmegaCommon)

OmegaWTKApp(
    NAME
    LayoutUnitTest
    BUNDLE_ID
    "org.omegagraphics.LayoutUnitTest"
    SOURCES
    LayoutUnitTest/main.cpp)

OmegaWTKApp(
    NAME
    LayoutResizeStressTest
    BUNDLE_ID
    "org.omegagraphics.LayoutResizeStressTest"
    SOURCES
    LayoutResizeStressTest/main.cpp)

OmegaWTKApp(
    NAME
    SVGViewRenderTest
    BUNDLE_ID
    "org.omegagraphics.SVGViewRenderTest"
    SOURCES
    SVGViewRenderTest/main.cpp)

# Tier 3 Phase 3.3: mirror the OmegaWTK_UI build flag onto the test
# binary so the same -DOMEGAWTK_WINDOW_SCOPED_PAINT=ON cmake option
# both (a) flips AppWindow::Impl::windowScopedPaint_'s default to
# true inside OmegaWTK_UI (which is what makes SVGView::paint() take
# the FrameBuilder route) and (b) lets this test's main.cpp report
# at startup which paint route the build is configured for.
if(OMEGAWTK_WINDOW_SCOPED_PAINT)
    target_compile_definitions(SVGViewRenderTest PRIVATE OMEGAWTK_WINDOW_SCOPED_PAINT=1)
endif()

# Tier 3 Phase 3.5 validator: drives a fabricated DisplayList with
# PushClip / draw-overflow / PopClip / draw-outside through
# DisplayListReplay into a real Canvas and visually verifies clip
# semantics. Mirrors the OMEGAWTK_WINDOW_SCOPED_PAINT flag onto the
# binary only so the startup log line can report which paint route
# is active; the test itself works in both modes (Canvas's SetClip
# emission is the same regardless of which canvas it lands on).
OmegaWTKApp(
    NAME
    DisplayListClipTest
    BUNDLE_ID
    "org.omegagraphics.DisplayListClipTest"
    SOURCES
    DisplayListClipTest/main.cpp)

if(OMEGAWTK_WINDOW_SCOPED_PAINT)
    target_compile_definitions(DisplayListClipTest PRIVATE OMEGAWTK_WINDOW_SCOPED_PAINT=1)
endif()

OmegaWTKApp(
    NAME
    VideoViewPlaybackTest
    BUNDLE_ID
    "org.omegagraphics.VideoViewPlaybackTest"
    SOURCES
    VideoViewPlaybackTest/main.cpp)

if(TARGET_MACOS)
    add_custom_target(2DAppTest DEPENDS EllipsePathCompositorTest.app)

    add_executable(MetalLayerTest MetalLayerTest/main.mm)
    target_link_libraries(MetalLayerTest PRIVATE OmegaGTE OmegaCommon)
    target_link_libraries(MetalLayerTest PRIVATE "-framework Cocoa" "-framework Metal" "-framework QuartzCore")
    target_include_directories(MetalLayerTest PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
    target_compile_definitions(MetalLayerTest PRIVATE TARGET_MACOS TARGET_METAL)
    if(TARGET OmegaGTE.framework)
        add_dependencies(MetalLayerTest OmegaGTE.framework)
    endif()
else()
    add_custom_target(2DAppTest DEPENDS EllipsePathCompositorTest)
endif()
