omega_graphics_project(KREATE VERSION 0.1 LANGUAGES CXX)

set(PLATFORM_SRCS)
if(WIN32)
    list(APPEND PLATFORM_SRCS src/platform/windows/Win32Window.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND PLATFORM_SRCS src/platform/ios/UIKitWindow.mm)
elseif(APPLE)
    list(APPEND PLATFORM_SRCS src/platform/macos/CocoaWindow.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    list(APPEND PLATFORM_SRCS src/platform/android/AndroidWindow.cpp)
else()
    find_package(X11 REQUIRED)
    list(APPEND PLATFORM_SRCS src/platform/linux/X11Window.cpp)
endif()

# KREATE shared library — glob per component so adding a new file inside
# src/<component>/ doesn't require touching this file. Platform sources
# stay explicit because the picker above selects exactly one.
file(GLOB KREATE_SRCS_TOPLEVEL CONFIGURE_DEPENDS src/*.cpp)
file(GLOB KREATE_SRCS_PIPELINE CONFIGURE_DEPENDS src/pipeline/*.cpp)
file(GLOB KREATE_SRCS_RENDERER CONFIGURE_DEPENDS src/renderer/*.cpp)

add_omega_graphics_module(KREATE SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${KREATE_SRCS_TOPLEVEL}
        ${KREATE_SRCS_PIPELINE}
        ${KREATE_SRCS_RENDERER}
        ${PLATFORM_SRCS}
    DEPENDS OmegaGTE
)
target_compile_definitions(KREATE PRIVATE KREATE__BUILD__)
target_link_libraries(KREATE PUBLIC OmegaGTE)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_system_frameworks(KREATE UIKit QuartzCore Metal Foundation)
    target_compile_definitions(KREATE PRIVATE TARGET_IOS TARGET_METAL)
elseif(APPLE)
    target_link_system_frameworks(KREATE Cocoa QuartzCore Metal)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(KREATE PRIVATE android log)
    target_compile_definitions(KREATE PRIVATE TARGET_ANDROID TARGET_VULKAN VULKAN_TARGET_ANDROID)
elseif(UNIX)
    target_include_directories(KREATE PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(KREATE PRIVATE ${X11_LIBRARIES})
endif()

# Game build function
include(cmake/KreateGame.cmake)

# BasicGame test. Android still ships a compile-only stub (no NativeActivity
# integration yet — see kreate/docs/Android-Host-Integration-Plan.md), so skip
# it there to avoid implying runnable behavior we don't have. iOS and the
# desktop platforms have real host integration.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_kreate_game(
        NAME BasicGame
        BUNDLE_ID "org.omegagraphics.kreate.BasicGame"
        SOURCES tests/BasicGame.cpp
    )
endif()

# Editor (future)
# if(COMMAND OmegaWTKApp)
#     OmegaWTKApp(
#         NAME
#         KREATEEditor
#         BUNDLE_ID
#         "org.omegagraphics.KREATEEditor"
#         SOURCES
#         editor/EditorMain.cpp)
#     target_link_libraries(KREATEEditor PRIVATE KREATE)
# endif()
