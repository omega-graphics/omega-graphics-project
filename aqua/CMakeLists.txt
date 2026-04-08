omega_graphics_project(AQUA VERSION 0.0 LANGUAGES CXX)

# add_omega_graphics_module("AQUA" STATIC SOURCES ${ENGINE_SRCS} ${CORE_SRCS} ${SCENE_SRCS} DEPENDS OmegaGTE)
# target_include_directories(AQUA PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine/include)
# target_link_libraries(AQUA PUBLIC OmegaGTE)

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
