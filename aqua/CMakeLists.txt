omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX)

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

# AQUA shared library — glob per component so adding a new file inside
# src/<component>/ doesn't require touching this file. Platform sources
# stay explicit because the picker above selects exactly one.
file(GLOB AQUA_SRCS_TOPLEVEL CONFIGURE_DEPENDS src/*.cpp)
file(GLOB AQUA_SRCS_PIPELINE CONFIGURE_DEPENDS src/pipeline/*.cpp)
file(GLOB AQUA_SRCS_RENDERER CONFIGURE_DEPENDS src/renderer/*.cpp)

add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${AQUA_SRCS_TOPLEVEL}
        ${AQUA_SRCS_PIPELINE}
        ${AQUA_SRCS_RENDERER}
        ${PLATFORM_SRCS}
    DEPENDS OmegaGTE
)
target_compile_definitions(AQUA PRIVATE AQUA__BUILD__)
target_link_libraries(AQUA PUBLIC OmegaGTE)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_system_frameworks(AQUA UIKit QuartzCore Metal Foundation)
    target_compile_definitions(AQUA PRIVATE TARGET_IOS TARGET_METAL)
elseif(APPLE)
    target_link_system_frameworks(AQUA Cocoa QuartzCore Metal)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(AQUA PRIVATE android log)
    target_compile_definitions(AQUA PRIVATE TARGET_ANDROID TARGET_VULKAN VULKAN_TARGET_ANDROID)
elseif(UNIX)
    target_include_directories(AQUA PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(AQUA PRIVATE ${X11_LIBRARIES})
endif()

# Game build function
include(cmake/AquaGame.cmake)

# BasicGame test. Android still ships a compile-only stub (no NativeActivity
# integration yet — see aqua/docs/Android-Host-Integration-Plan.md), so skip
# it there to avoid implying runnable behavior we don't have. iOS and the
# desktop platforms have real host integration.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_aqua_game(
        NAME BasicGame
        BUNDLE_ID "org.omegagraphics.aqua.BasicGame"
        SOURCES tests/BasicGame.cpp
    )
endif()

# Editor (future)
# if(COMMAND OmegaWTKApp)
#     OmegaWTKApp(
#         NAME
#         AQUAEditor
#         BUNDLE_ID
#         "org.omegagraphics.AQUAEditor"
#         SOURCES
#         editor/EditorMain.cpp)
#     target_link_libraries(AQUAEditor PRIVATE AQUA)
# endif()
