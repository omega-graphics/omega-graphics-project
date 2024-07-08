include(CMakeParseArguments)

pkg_check_modules(GTK gtk+-3.0)

function(add_vulkan_test)
    cmake_parse_arguments("_ARG" "" "NAME" ";SOURCES;LIBS" ${ARGN})
    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    target_include_directories(${_ARG_NAME} PUBLIC ${GTK_INCLUDE_DIRS})
    target_link_directories(${_ARG_NAME} PRIVATE ${GTK_LIBRARY_DIRS})
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${GTK_LIBRARIES})
endfunction()

add_vulkan_test(
    NAME
    "2DTest"
    SOURCES 
    "2DTest/main.cpp")
