omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX)

set(PLATFORM_SRCS)
if(WIN32)
    list(APPEND PLATFORM_SRCS src/platform/Win32Window.cpp)
elseif(APPLE)
    list(APPEND PLATFORM_SRCS src/platform/CocoaWindow.mm)
else()
    list(APPEND PLATFORM_SRCS src/platform/X11Window.cpp)
endif()

# AQUA shared library
add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES src/App.cpp ${PLATFORM_SRCS}
    DEPENDS OmegaGTE
)
target_compile_definitions(AQUA PRIVATE AQUA__BUILD__)
target_link_libraries(AQUA PUBLIC OmegaGTE)

if(APPLE)
    target_link_system_frameworks(AQUA Cocoa QuartzCore Metal)
endif()

# Game build function
include(cmake/AquaGame.cmake)

# BasicGame test
add_aqua_game(
    NAME BasicGame
    BUNDLE_ID "org.omegagraphics.aqua.BasicGame"
    SOURCES tests/BasicGame.cpp
)

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
