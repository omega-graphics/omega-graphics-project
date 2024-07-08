cmake_minimum_required(VERSION 3.9)

set(OMEGACOMMON_VERSION "0.5")

include(OmegaGraphicsSuite)

omega_graphics_project(OmegaCommon VERSION ${OMEGACOMMON_VERSION} LANGUAGES C CXX)


set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/cmake)

include(OmegaGraphicsSuite)

set(COMMON_SRCS "src/crt.c" "src/format.cpp" "src/json.cpp" "src/fs.cpp" "src/utils.cpp")

configure_file("src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")


add_omega_graphics_module("OmegaCommon" SHARED SOURCES ${COMMON_SRCS} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
	target_sources("OmegaCommon" PRIVATE "src/fs-win.cpp"  "src/net-win.cpp" "src/multithread-win.cpp")
	target_compile_definitions("OmegaCommon" PRIVATE "OMEGACOMMON__BUILD__")
elseif(APPLE)
	target_sources("OmegaCommon" PRIVATE "src/fs-cocoa.mm"  "src/net-curl.cpp" "src/multithread-unix.cpp")
	target_link_system_frameworks("OmegaCommon" "Foundation")
	target_link_libraries("OmegaCommon" PRIVATE curl)
elseif(UNIX)
	target_sources("OmegaCommon" PRIVATE "src/fs-unixother.cpp" "src/multithread-unix.cpp")
endif()

file(GLOB WRAPGEN_SRCS "wrapgen/*.cpp")

add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/main/main.cpp")

add_omega_graphics_tool("lex-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/lex-test.cpp")

add_omega_graphics_tool("parse-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/parse-test.cpp")

file(GLOB EBIN_SRCS "embedbin/*.cpp")

add_omega_graphics_tool("omega-ebin" LIBS "OmegaCommon" SOURCES ${EBIN_SRCS})


