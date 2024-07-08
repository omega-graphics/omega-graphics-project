cmake_minimum_required(VERSION 3.13)

include(cmake/OmegaGraphicsSuite.cmake)

project(OmegaGraphics C CXX)

# Allow Objecitve C /Objetive C++ to be built for any projects
if(APPLE)
    enable_language(OBJCXX)
endif()

omega_graphics_add_subdir(AUTOM autom)

omega_graphics_add_subdir(OmegaCommon common)

omega_graphics_add_subdir(OmegaGTE gte)

omega_graphics_add_subdir(OmegaWTK wtk)