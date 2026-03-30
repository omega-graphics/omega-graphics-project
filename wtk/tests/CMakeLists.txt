# add_subdirectory("RootWidget")
# # if(SWIFT_BUILD)
# #     add_subdirectory("SwiftImport")
# # endif()

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
    ContainerClampAnimationTest
    BUNDLE_ID
    "org.omegagraphics.ContainerClampAnimationTest"
    SOURCES
    ContainerClampAnimationTest/main.cpp)

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
