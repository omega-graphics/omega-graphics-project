# cmake_minimum_required(VERSION 3.9)

set(OMEGACOMMON_VERSION "0.5")

omega_graphics_project(OmegaCommon VERSION ${OMEGACOMMON_VERSION} LANGUAGES C CXX)


set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/cmake)

file(GLOB COMMON_SRCS "src/*.cpp")

configure_file("src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")


add_omega_graphics_module("OmegaCommon" SHARED SOURCES ${COMMON_SRCS} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
	target_sources("OmegaCommon" PRIVATE "src/win/fs-win.cpp"  "src/win/net-win.cpp" "src/win/multithread-win.cpp")
	target_compile_definitions("OmegaCommon" PRIVATE "OMEGACOMMON__BUILD__")
elseif(APPLE)
	target_sources("OmegaCommon" PRIVATE "src/macos/fs-cocoa.mm"  "src/macos/net-curl.cpp" "src/unix/multithread-unix.cpp")
	target_link_system_frameworks("OmegaCommon" "Foundation")
	target_link_libraries("OmegaCommon" PRIVATE curl)
elseif(UNIX)
	target_sources("OmegaCommon" PRIVATE "src/unix/fs-unixother.cpp" "src/unix/net-unix.cpp" "src/unix/multithread-unix.cpp")
endif()

file(GLOB WRAPGEN_SRCS "wrapgen/*.cpp")

add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/main/main.cpp")

add_omega_graphics_tool("omegawtk-assetc" LIBS "OmegaCommon" SOURCES assetc/assetc.h assetc/main.cpp)

# add_omega_graphics_tool("lex-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/lex-test.cpp")

add_omega_graphics_tool("parse-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/parse-test.cpp")

add_custom_target(wrapgen-tests
    COMMAND ${PYEXEC} ${CMAKE_CURRENT_SOURCE_DIR}/wrapgen/tests/run_tests.py
        --omega-wrapgen ${CMAKE_BINARY_DIR}/bin/omega-wrapgen
        --suite-dir ${CMAKE_CURRENT_SOURCE_DIR}/wrapgen/tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS omega-wrapgen
    COMMENT "Running omega-wrapgen integration tests")

file(GLOB EBIN_SRCS "embedbin/*.cpp")

add_omega_graphics_tool("omega-ebin" LIBS "OmegaCommon" SOURCES ${EBIN_SRCS})
