omega_graphics_project(AQUA VERSION 0.0 LANGUAGES CXX)

file(GLOB CORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/engine/src/Core/*.cpp)
source_group(AQUACore ${CORE_SRCS})


file(GLOB PHYSICS_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/engine/src/Physics/*.cpp)
source_group(AQUAPhysics ${PHYSICS_SRCS})

file(GLOB SCENE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/engine/src/Scene/*.cpp)
source_group(AQUAScene ${SCENE_SRCS})


add_omega_graphics_module("AQUA" STATIC SOURCES ${CORE_SRCS} ${SCENE_SRCS} DEPENDS OmegaGTE)
target_include_directories(AQUA PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine/include)

