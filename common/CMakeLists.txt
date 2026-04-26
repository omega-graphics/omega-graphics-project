# cmake_minimum_required(VERSION 3.9)

set(OMEGACOMMON_VERSION "0.5")

omega_graphics_project(OmegaCommon VERSION ${OMEGACOMMON_VERSION} LANGUAGES C CXX)


set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(OMEGACOMMON_AUTOMDEPS_CMAKE_CANDIDATES
	"${CMAKE_CURRENT_SOURCE_DIR}/.automdeps/exports.cmake"
	"${CMAKE_SOURCE_DIR}/.automdeps/exports.cmake")
foreach(OMEGACOMMON_AUTOMDEPS_CMAKE IN LISTS OMEGACOMMON_AUTOMDEPS_CMAKE_CANDIDATES)
	if(EXISTS "${OMEGACOMMON_AUTOMDEPS_CMAKE}")
		include("${OMEGACOMMON_AUTOMDEPS_CMAKE}")
		break()
	endif()
endforeach()
unset(OMEGACOMMON_AUTOMDEPS_CMAKE)
unset(OMEGACOMMON_AUTOMDEPS_CMAKE_CANDIDATES)

file(GLOB COMMON_SRCS CONFIGURE_DEPENDS "src/*.cpp")

configure_file("src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")

if(NOT WIN32)
	set(OMEGACOMMON_CURL_TARGET "")
	set(OMEGACOMMON_CURL_INCLUDE_DIRS "")
	set(OMEGACOMMON_CURL_LIBRARIES "")
	# macOS/iOS has libcurl as part of the system.
	
	if(NOT APPLE)
		find_package(CURL QUIET)
		if(TARGET CURL::libcurl)
			set(OMEGACOMMON_CURL_TARGET CURL::libcurl)
		elseif(CURL_FOUND)
			set(OMEGACOMMON_CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIRS})
			set(OMEGACOMMON_CURL_LIBRARIES ${CURL_LIBRARIES})
		endif()

		if((UNIX AND NOT APPLE) AND NOT OMEGACOMMON_CURL_TARGET AND NOT OMEGACOMMON_CURL_LIBRARIES)
			find_package(PkgConfig QUIET)
			if(PkgConfig_FOUND)
				pkg_check_modules(LIBCURL QUIET IMPORTED_TARGET libcurl)
				if(TARGET PkgConfig::LIBCURL)
					set(OMEGACOMMON_CURL_TARGET PkgConfig::LIBCURL)
				endif()
			endif()
		endif()

		if(NOT OMEGACOMMON_CURL_TARGET AND NOT OMEGACOMMON_CURL_LIBRARIES)
			message(FATAL_ERROR "OmegaCommon requires libcurl on non-Windows platforms. Install the libcurl development package so CMake or pkg-config can find it.")
		endif()
	else()
		# On Apple platforms, use the system libcurl. The find_package(CURL) approach doesn't work well there because the system libcurl is a "framework" and CMake's FindCURL module doesn't handle that well.
		set(OMEGACOMMON_CURL_LIBRARIES "-lcurl")
	endif()
endif()

# --- Third-party: PCRE2 ---
set(PCRE2_VERSION "10.47")
if(DEFINED OMEGACOMMON_PCRE2_SOURCE_ROOT)
	set(PCRE2_SOURCE_DIR "${OMEGACOMMON_PCRE2_SOURCE_ROOT}")
else()
	set(PCRE2_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/pcre2/code/pcre2-${PCRE2_VERSION}")
endif()
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

if(WIN32)
	# PCRE2's Windows headers default to dllimport/dllexport semantics unless
	# static consumers define PCRE2_STATIC. Propagate that from the imported target.
	set_property(TARGET pcre2-8 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS PCRE2_STATIC)
endif()

# --- Third-party: OpenSSL ---
if(DEFINED OMEGACOMMON_OPENSSL_ROOT)
	set(OPENSSL_SOURCE_DIR "${OMEGACOMMON_OPENSSL_ROOT}")
else()
	set(OPENSSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/openssl/code")
endif()
set(OPENSSL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/deps/openssl")
set(OMEGACOMMON_OPENSSL_PERL)

if(WIN32)
	if(DEFINED OMEGACOMMON_STRAWBERRY_PERL_PERL AND EXISTS "${OMEGACOMMON_STRAWBERRY_PERL_PERL}")
		set(OMEGACOMMON_OPENSSL_PERL "${OMEGACOMMON_STRAWBERRY_PERL_PERL}")
	else()
		set(OMEGACOMMON_STRAWBERRY_PERL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/strawberry-perl")
		set(_OMEGACOMMON_STRAWBERRY_PERL_CANDIDATES
			"${OMEGACOMMON_STRAWBERRY_PERL_DIR}/perl/bin/perl.exe")
		file(GLOB _OMEGACOMMON_STRAWBERRY_PERL_NESTED
			LIST_DIRECTORIES FALSE
			"${OMEGACOMMON_STRAWBERRY_PERL_DIR}/*/perl/bin/perl.exe")
		list(APPEND _OMEGACOMMON_STRAWBERRY_PERL_CANDIDATES ${_OMEGACOMMON_STRAWBERRY_PERL_NESTED})

		foreach(_OMEGACOMMON_PERL_CANDIDATE IN LISTS _OMEGACOMMON_STRAWBERRY_PERL_CANDIDATES)
			if(EXISTS "${_OMEGACOMMON_PERL_CANDIDATE}")
				set(OMEGACOMMON_OPENSSL_PERL "${_OMEGACOMMON_PERL_CANDIDATE}")
				break()
			endif()
		endforeach()

		unset(_OMEGACOMMON_PERL_CANDIDATE)
		unset(_OMEGACOMMON_STRAWBERRY_PERL_CANDIDATES)
		unset(_OMEGACOMMON_STRAWBERRY_PERL_NESTED)
	endif()

	if(NOT OMEGACOMMON_OPENSSL_PERL)
		find_program(OMEGACOMMON_OPENSSL_PERL NAMES perl perl.exe)
	endif()
else()
	find_program(OMEGACOMMON_OPENSSL_PERL NAMES perl)
endif()

if(NOT OMEGACOMMON_OPENSSL_PERL)
	message(FATAL_ERROR "OpenSSL configuration requires Perl. On Windows run AUTOMDEPS to fetch Strawberry Perl; on other platforms install perl.")
endif()

message(STATUS "OmegaCommon OpenSSL Configure Perl: ${OMEGACOMMON_OPENSSL_PERL}")

if(WIN32)
	add_third_party(
		CUSTOM_PROJECT
		NAME openssl
		SOURCE_DIR "${OPENSSL_SOURCE_DIR}"
		BINARY_DIR "${OPENSSL_SOURCE_DIR}"
		INSTALL_DIR "${OPENSSL_OUTPUT_DIR}"
		CONF "${OMEGACOMMON_OPENSSL_PERL}" "${OPENSSL_SOURCE_DIR}/Configure" --prefix=${OPENSSL_OUTPUT_DIR} no-shared no-tests no-apps VC-WIN64A
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
		CONF "${OMEGACOMMON_OPENSSL_PERL}" "${OPENSSL_SOURCE_DIR}/Configure" --prefix=${OPENSSL_OUTPUT_DIR} --libdir=lib no-shared no-tests no-apps
		BUILD make
		INSTALL make install_sw
		EXPORT_STATIC_LIBS "crypto:lib/libcrypto.a:lib/libcrypto.lib" "ssl:lib/libssl.a:lib/libssl.lib"
		EXPORT_INCLUDE_DIRS "include")
endif()

add_omega_graphics_module("OmegaCommon" SHARED SOURCES ${COMMON_SRCS} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(DEFINED OMEGACOMMON_RAPIDJSON_ROOT)
	set(OMEGACOMMON_RAPIDJSON_INCLUDE_DIR "${OMEGACOMMON_RAPIDJSON_ROOT}/include")
else()
	set(OMEGACOMMON_RAPIDJSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/rapidjson/include")
endif()
target_include_directories("OmegaCommon" PRIVATE "${OMEGACOMMON_RAPIDJSON_INCLUDE_DIR}")

target_link_libraries("OmegaCommon" PRIVATE pcre2-8 ssl crypto)
if(NOT WIN32)
	if(OMEGACOMMON_CURL_TARGET)
		target_link_libraries("OmegaCommon" PRIVATE ${OMEGACOMMON_CURL_TARGET})
	else()
		target_include_directories("OmegaCommon" PRIVATE ${OMEGACOMMON_CURL_INCLUDE_DIRS})
		target_link_libraries("OmegaCommon" PRIVATE ${OMEGACOMMON_CURL_LIBRARIES})
	endif()
endif()

if(WIN32)
	target_sources("OmegaCommon" PRIVATE "src/win/fs-win.cpp"  "src/win/net-win.cpp" "src/win/multithread-win.cpp")
	target_compile_definitions("OmegaCommon" PRIVATE "OMEGACOMMON__BUILD__")
	target_link_libraries("OmegaCommon" PRIVATE ws2_32 advapi32 crypt32 user32)
elseif(APPLE)
	target_sources("OmegaCommon" PRIVATE "src/macos/fs-cocoa.mm" "src/posix/net-curl.cpp" "src/unix/multithread-unix.cpp")
	target_link_system_frameworks("OmegaCommon" "Foundation")
elseif(UNIX)
	
	target_sources("OmegaCommon" PRIVATE "src/unix/fs-unixother.cpp" "src/posix/net-curl.cpp" "src/unix/multithread-unix.cpp")
	target_link_libraries("OmegaCommon" PRIVATE dl pthread)
endif()

file(GLOB WRAPGEN_SRCS "wrapgen/*.cpp")

add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/main/main.cpp")

add_omega_graphics_tool("omega-assetc" LIBS "OmegaCommon" SOURCES assetc/assetc.h assetc/main.cpp)
add_custom_command(TARGET omega-assetc POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/assetc/AssetTypes.json"
		"$<TARGET_FILE_DIR:omega-assetc>/AssetTypes.json")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assetc/AssetTypes.json" DESTINATION bin)

add_subdirectory(assetc/tests)

# add_omega_graphics_tool("lex-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/lex-test.cpp")

add_omega_graphics_tool("parse-test" LIBS "OmegaCommon" SOURCES ${WRAPGEN_SRCS} "wrapgen/tests/parse-test.cpp")

add_custom_target(wrapgen-tests
    COMMAND ${PYEXEC} ${CMAKE_CURRENT_SOURCE_DIR}/wrapgen/tests/run_tests.py
        --omega-wrapgen ${CMAKE_BINARY_DIR}/bin/omega-wrapgen
        --parse-test ${CMAKE_BINARY_DIR}/bin/parse-test
        --suite-dir ${CMAKE_CURRENT_SOURCE_DIR}/wrapgen/tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS omega-wrapgen parse-test
    COMMENT "Running omega-wrapgen integration tests")

file(GLOB EBIN_SRCS "embedbin/*.cpp")

add_omega_graphics_tool("omega-ebin" LIBS "OmegaCommon" SOURCES ${EBIN_SRCS})
