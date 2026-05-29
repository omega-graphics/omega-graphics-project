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

# Tier 4 §4.2: the DisplayListClipTest / ScrollViewClipTest /
# NativeContentCarveoutTest validators were retired here — they drove the
# deleted DisplayListReplay → Canvas bridge and consumed VisualCommand /
# CanvasFrame / makeCanvas (all gone in 4.2), and tests cannot reach the
# private FrameBuilder to inject a hand-built DisplayList the new way. The
# clip path is exercised by the real ScrollView; carve-out by video.

OmegaWTKApp(
    NAME
    VideoViewPlaybackTest
    BUNDLE_ID
    "org.omegagraphics.VideoViewPlaybackTest"
    SOURCES
    VideoViewPlaybackTest/main.cpp)

