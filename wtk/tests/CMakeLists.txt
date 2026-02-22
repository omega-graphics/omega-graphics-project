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
    BasicAppTest/main.cpp)

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

if(TARGET_MACOS)
    add_custom_target(2DAppTest DEPENDS EllipsePathCompositorTest.app)
else()
    add_custom_target(2DAppTest DEPENDS EllipsePathCompositorTest)
endif()
