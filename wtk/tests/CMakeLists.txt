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

if(TARGET_MACOS)
    add_custom_target(2DAppTest DEPENDS BasicAppTest.app)
else()
    add_custom_target(2DAppTest DEPENDS BasicAppTest)
endif()
