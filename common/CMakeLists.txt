# cmake_minimum_required(VERSION 3.9)

set(OMEGACOMMON_VERSION "0.5")

omega_graphics_project(OmegaCommon VERSION ${OMEGACOMMON_VERSION} LANGUAGES C CXX)


set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/cmake)

file(GLOB COMMON_SRCS "src/*.cpp")

configure_file("src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")

# --- Third-party: PCRE2 ---
set(PCRE2_VERSION "10.38")
set(PCRE2_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/pcre2/code/pcre2-${PCRE2_VERSION}")
set(PCRE2_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/deps/pcre2")

add_third_party(
	NAME pcre2
	SOURCE_DIR "${PCRE2_SOURCE_DIR}"
	BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/deps/pcre2-build"
	INSTALL_DIR "${PCRE2_OUTPUT_DIR}"
	CMAKE_BUILD_ARGS
		-DPCRE2_BUILD_PCRE2_8=ON
		-DPCRE2_BUILD_PCRE2_16=OFF
		-DPCRE2_BUILD_PCRE2_32=OFF
		-DPCRE2_BUILD_TESTS=OFF
		-DPCRE2_BUILD_PCRE2GREP=OFF
		-DBUILD_SHARED_LIBS=OFF
	EXPORT_STATIC_LIBS "pcre2-8:lib/libpcre2-8.a:lib/pcre2-8-static.lib"
	EXPORT_INCLUDE_DIRS "include"
)

# --- Third-party: OpenSSL ---
set(OPENSSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/openssl/code")
set(OPENSSL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/deps/openssl")

if(WIN32)
	add_third_party(
		CUSTOM_PROJECT
		NAME openssl
		SOURCE_DIR "${OPENSSL_SOURCE_DIR}"
		BINARY_DIR "${OPENSSL_SOURCE_DIR}"
		INSTALL_DIR "${OPENSSL_OUTPUT_DIR}"
		CONF perl "${OPENSSL_SOURCE_DIR}/Configure" --prefix=${OPENSSL_OUTPUT_DIR} no-shared no-tests no-apps VC-WIN64A
		BUILD nmake
		INSTALL nmake install_sw
		EXPORT_STATIC_LIBS "crypto:lib/libcrypto.a:lib/libcrypto.lib" "ssl:lib/libssl.a:lib/libssl.lib"
		EXPORT_INCLUDE_DIRS "include")
else()
	add_third_party(
		CUSTOM_PROJECT
		NAME openssl
		SOURCE_DIR "${OPENSSL_SOURCE_DIR}"
		BINARY_DIR "${OPENSSL_SOURCE_DIR}"
		INSTALL_DIR "${OPENSSL_OUTPUT_DIR}"
		CONF perl "${OPENSSL_SOURCE_DIR}/Configure" --prefix=${OPENSSL_OUTPUT_DIR} no-shared no-tests no-apps
		BUILD make
		INSTALL make install_sw
		EXPORT_STATIC_LIBS "crypto:lib/libcrypto.a:lib/libcrypto.lib" "ssl:lib/libssl.a:lib/libssl.lib"
		EXPORT_INCLUDE_DIRS "include")
endif()

add_omega_graphics_module("OmegaCommon" SHARED SOURCES ${COMMON_SRCS} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries("OmegaCommon" PRIVATE pcre2-8 ssl crypto)
if(WIN32)
	target_sources("OmegaCommon" PRIVATE "src/win/fs-win.cpp"  "src/win/net-win.cpp" "src/win/multithread-win.cpp")
	target_compile_definitions("OmegaCommon" PRIVATE "OMEGACOMMON__BUILD__")
	target_link_libraries("OmegaCommon" PRIVATE ws2_32 advapi32 crypt32 user32)
elseif(APPLE)
	target_sources("OmegaCommon" PRIVATE "src/macos/fs-cocoa.mm" "src/posix/net-curl.cpp" "src/unix/multithread-unix.cpp")
	target_link_system_frameworks("OmegaCommon" "Foundation")
	target_link_libraries("OmegaCommon" PRIVATE curl)
elseif(UNIX)
	target_sources("OmegaCommon" PRIVATE "src/unix/fs-unixother.cpp" "src/posix/net-curl.cpp" "src/unix/multithread-unix.cpp")
	target_link_libraries("OmegaCommon" PRIVATE curl dl pthread)
endif()

file(GLOB WRAPGEN_SRCS "wrapgen/*.cpp")

add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/main/main.cpp")

add_omega_graphics_tool("omega-assetc" LIBS "OmegaCommon" SOURCES assetc/assetc.h assetc/main.cpp)

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
