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

file(GLOB COMMON_SRCS         CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB COMMON_UNICODE_SRCS CONFIGURE_DEPENDS "src/unicode/*.cpp")
file(GLOB COMMON_IMG_SRCS     CONFIGURE_DEPENDS "src/img/*.cpp")
list(APPEND COMMON_SRCS ${COMMON_UNICODE_SRCS} ${COMMON_IMG_SRCS})

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
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON
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
		CONF "${OMEGACOMMON_OPENSSL_PERL}" "${OPENSSL_SOURCE_DIR}/Configure" --prefix=${OPENSSL_OUTPUT_DIR} --libdir=lib no-shared no-tests no-apps -fPIC
		BUILD make
		INSTALL make install_sw
		EXPORT_STATIC_LIBS "crypto:lib/libcrypto.a:lib/libcrypto.lib" "ssl:lib/libssl.a:lib/libssl.lib"
		EXPORT_INCLUDE_DIRS "include")
endif()

# === Image-codec + ICU third-party deps ===
# Migrated from wtk/CMakeLists.txt in Phase 1a of the
# Common-ImgCodec-Unicode-Refactor-Plan. wtk/CMakeLists.txt and any other
# subdirectory still references the imported targets (icuuc, icudata,
# icui18n, png, turbojpeg, tiff, z) and the *_EXPORT / *_DLL CACHE INTERNAL
# variables defined below. Image deps (libpng, libjpeg-turbo, libtiff, zlib)
# remain SHARED in Phase 1a and switch to STATIC in Phase 1b.
set(OMEGACOMMON_THIRD_PARTY_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/deps")
set(OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/deps" CACHE INTERNAL "Common-owned third-party install root (consumed by wtk libxml2 build)" FORCE)

# --- ICU ---
set(ICU_DEP_ROOT    "${OMEGACOMMON_THIRD_PARTY_SRC_DIR}/icu")
set(ICU_SOURCE_ROOT "${ICU_DEP_ROOT}/code")
set(_OMEGACOMMON_ICU_VERSION "78")
set(ICU_VERSION_HEADER "${ICU_SOURCE_ROOT}/icu4c/source/common/unicode/uvernum.h")
if(EXISTS "${ICU_VERSION_HEADER}")
	file(STRINGS "${ICU_VERSION_HEADER}" ICU_VERSION_LINE
		REGEX "^#define U_ICU_VERSION_SHORT[ \t]+\"[0-9]+\"")
	if(ICU_VERSION_LINE)
		string(REGEX REPLACE ".*\"([0-9]+)\".*" "\\1" _OMEGACOMMON_ICU_VERSION "${ICU_VERSION_LINE}")
	endif()
endif()
set(ICU_VERSION "${_OMEGACOMMON_ICU_VERSION}" CACHE INTERNAL "ICU short version (e.g. 78); consumed by wtk's framework install-name fixups")

if(WIN32)
	file(MAKE_DIRECTORY "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin")
	file(MAKE_DIRECTORY "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib")

	# Map CMake target arch -> ICU msbuild Platform + bin/lib subdir.
	# Prefer the VS-generator value when set; otherwise fall back to pointer
	# width / CMAKE_SYSTEM_PROCESSOR. ICU's bin/lib directory names mirror
	# msbuild Platform: Win32->bin/lib, x64->bin64/lib64, ARM->binARM/libARM,
	# ARM64->binARM64/libARM64 (see source/data/makedata.mak and the
	# allinone Build.Windows.ProjectConfiguration.props).
	if(CMAKE_GENERATOR_PLATFORM)
		set(_ICU_MSBUILD_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
		set(_ICU_MSBUILD_PLATFORM "ARM64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM|arm)$")
		set(_ICU_MSBUILD_PLATFORM "ARM")
	elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(_ICU_MSBUILD_PLATFORM "x64")
	else()
		set(_ICU_MSBUILD_PLATFORM "Win32")
	endif()

	if(_ICU_MSBUILD_PLATFORM STREQUAL "x64")
		set(_ICU_BIN_SUBDIR "bin64")
		set(_ICU_LIB_SUBDIR "lib64")
	elseif(_ICU_MSBUILD_PLATFORM STREQUAL "ARM64")
		set(_ICU_BIN_SUBDIR "binARM64")
		set(_ICU_LIB_SUBDIR "libARM64")
	elseif(_ICU_MSBUILD_PLATFORM STREQUAL "ARM")
		set(_ICU_BIN_SUBDIR "binARM")
		set(_ICU_LIB_SUBDIR "libARM")
	else()
		set(_ICU_BIN_SUBDIR "bin")
		set(_ICU_LIB_SUBDIR "lib")
	endif()

	# Size guard for the data DLL. The stubdata.vcxproj DLL is ~3.5 KB; a
	# real makedata-built icudt78.dll is ~28 MB. Anything under 1 MiB means
	# pkgdata didn't run (commonly: msbuild was invoked without an explicit
	# /p:Platform, or `py -3` is not on PATH -- see makedata.mak:36).
	set(_ICU_DATA_DLL_MIN_BYTES 1048576)

	add_third_party(
		CUSTOM_PROJECT
		NAME icu
		SOURCE_DIR "${ICU_SOURCE_ROOT}/icu4c/source/allinone"
		BINARY_DIR "${ICU_SOURCE_ROOT}/icu4c/source/allinone"
		INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu"
		CONF ""
		BUILD msbuild allinone.sln /p:Configuration=Release /p:Platform=${_ICU_MSBUILD_PLATFORM} /p:SkipUWP=true
		INSTALL ${CMAKE_COMMAND} -E copy_directory "${ICU_SOURCE_ROOT}/icu4c/include" "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/include"
		&& ${CMAKE_COMMAND} -E copy "${ICU_SOURCE_ROOT}/icu4c/${_ICU_BIN_SUBDIR}/icuuc${ICU_VERSION}.dll" "${ICU_SOURCE_ROOT}/icu4c/${_ICU_BIN_SUBDIR}/icudt${ICU_VERSION}.dll" "${ICU_SOURCE_ROOT}/icu4c/${_ICU_BIN_SUBDIR}/icuin${ICU_VERSION}.dll" "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin"
		&& ${CMAKE_COMMAND} -E copy "${ICU_SOURCE_ROOT}/icu4c/${_ICU_LIB_SUBDIR}/icuuc.lib" "${ICU_SOURCE_ROOT}/icu4c/${_ICU_LIB_SUBDIR}/icudt.lib" "${ICU_SOURCE_ROOT}/icu4c/${_ICU_LIB_SUBDIR}/icuin.lib" "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib"
		&& ${CMAKE_COMMAND} -DFILE=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin/icudt${ICU_VERSION}.dll -DMIN_SIZE=${_ICU_DATA_DLL_MIN_BYTES} -DLABEL=icudt${ICU_VERSION}.dll -P ${CMAKE_SOURCE_DIR}/cmake/CheckMinFileSize.cmake
		EXPORT_SHARED_LIBS "icuuc:lib/icuuc.lib" "icudata:lib/icudt.lib" "icui18n:lib/icuin.lib"
		EXPORT_INCLUDE_DIRS "include")

	set(ICU_EXPORT
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/icuuc.lib"
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/icudt.lib"
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/icuin.lib"
		CACHE INTERNAL "ICU import libraries (consumed by OmegaWTK framework EMBEDDED_LIBS)")
	set(ICU_DLLS
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin/icuuc${ICU_VERSION}.dll"
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin/icudt${ICU_VERSION}.dll"
		"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/bin/icuin${ICU_VERSION}.dll"
		CACHE INTERNAL "ICU runtime DLLs (consumed by OmegaWTK on Win32)")
else()

	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		add_third_party(
			CUSTOM_PROJECT
			NAME icu_tools
			SOURCE_DIR "${ICU_DEP_ROOT}"
			BINARY_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/intermed/icu_mac"
			INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/intermed/icu_mac"
			CONF cd "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/intermed/icu_mac" &&  ${ICU_SOURCE_ROOT}/icu4c/source/configure --prefix=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu
			BUILD make all
			INSTALL echo "ICU Cross Tools Complete")

		add_third_party(
			CUSTOM_PROJECT
			NAME icu
			SOURCE_DIR "${ICU_DEP_ROOT}"
			BINARY_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/intermed/icu_ios"
			INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu"
			CONF
			export CFLAGS=--sysroot=${CMAKE_OSX_SYSROOT} &&
			export CXXFLAGS=--sysroot=${CMAKE_OSX_SYSROOT} &&
			${ICU_SOURCE_ROOT}/icu4c/source/configure --prefix=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu --with-cross-build=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/intermed/icu_mac
			BUILD make all
			INSTALL make install
			EXPORT_SHARED_LIBS "icuuc:lib/libicuuc${CMAKE_SHARED_LIBRARY_SUFFIX}" "icudata:lib/libicudata${CMAKE_SHARED_LIBRARY_SUFFIX}" "icui18n:lib/libicui18n${CMAKE_SHARED_LIBRARY_SUFFIX}"
			EXPORT_INCLUDE_DIRS "include"
			DEPS icu_tools)
	else()
		add_third_party(
			CUSTOM_PROJECT
			NAME icu
			SOURCE_DIR "${ICU_SOURCE_ROOT}/icu4c/source"
			BINARY_DIR "${ICU_SOURCE_ROOT}/icu4c/source"
			INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu"
			CONF  ${ICU_SOURCE_ROOT}/icu4c/source/configure --prefix=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu
			BUILD make all
			INSTALL make install
			EXPORT_SHARED_LIBS "icuuc:lib/libicuuc${CMAKE_SHARED_LIBRARY_SUFFIX}" "icudata:lib/libicudata${CMAKE_SHARED_LIBRARY_SUFFIX}" "icui18n:lib/libicui18n${CMAKE_SHARED_LIBRARY_SUFFIX}"
			EXPORT_INCLUDE_DIRS "include")
	endif()

	if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
		set(ICU_EXPORT
			${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicuuc.dylib
			${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicudata.dylib
			${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.dylib
			CACHE INTERNAL "ICU dylib paths")
		set_library_install_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicuuc.dylib"   "@rpath/libicuuc.dylib"   AFTER icu)
		set_library_install_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicudata.dylib" "@rpath/libicudata.dylib" AFTER icu)
		set_library_install_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.dylib" "@rpath/libicui18n.dylib" AFTER icu)
		reset_library_dependent_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicuuc.dylib"   "libicudata.${ICU_VERSION}.dylib" "@rpath/libicudata.dylib" AFTER icu)
		reset_library_dependent_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.dylib" "libicudata.${ICU_VERSION}.dylib" "@rpath/libicudata.dylib" AFTER icu)
		reset_library_dependent_name("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.dylib" "libicuuc.${ICU_VERSION}.dylib"   "@rpath/libicuuc.dylib"   AFTER icu)
		# Add @loader_path rpath so co-located dylibs can find each other at runtime
		add_library_rpath("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicuuc.dylib"   "@loader_path" AFTER icu)
		add_library_rpath("${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.dylib" "@loader_path" AFTER icu)
	elseif(UNIX AND NOT APPLE AND NOT ANDROID)
		set(ICU_EXPORT
			"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicuuc.so"
			"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicudata.so"
			"${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/lib/libicui18n.so"
			CACHE INTERNAL "ICU shared library paths")
	endif()

endif()

# Per-platform runtime artifact list, used by add_omega_graphics_module's
# EMBEDDED_LIBS handling: import libs (.lib) on Win32 are not loadable, so the
# embed must point at the actual .dll. On macOS / Linux the dylib / .so is
# both the link-time and load-time artifact.
if(WIN32)
	set(ICU_RUNTIME "${ICU_DLLS}"   CACHE INTERNAL "ICU runtime artifacts (DLLs on Win32)")
else()
	set(ICU_RUNTIME "${ICU_EXPORT}" CACHE INTERNAL "ICU runtime artifacts (dylib/.so on Mac/Linux)")
endif()

# --- zlib (static) ---
# zlib's CMakeLists always builds both `zlib` (shared) and `zlibstatic` (static)
# regardless of BUILD_SHARED_LIBS — we just consume the static archive. Static
# install paths: lib/libz.a on UNIX (OUTPUT_NAME=z, prefix=lib), lib/zlibstatic.lib
# on Windows (target name `zlibstatic`).
add_third_party(
	NAME zlib
	SOURCE_DIR "${OMEGACOMMON_THIRD_PARTY_SRC_DIR}/zlib/code"
	BINARY_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/temp/zlib"
	INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/zlib"
	CMAKE_BUILD_ARGS -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	EXPORT_STATIC_LIBS "z:lib/libz.a:lib/zlibstatic.lib"
	EXPORT_INCLUDE_DIRS "include")

get_target_property(_OMEGACOMMON_ZLIB_INCLUDE_DIRS z INTERFACE_INCLUDE_DIRECTORIES)

# --- libpng (static) ---
# libpng has its own PNG_SHARED / PNG_STATIC options (both default ON); pass
# PNG_SHARED=OFF to skip the shared build entirely. Static install paths: on
# UNIX, libpng creates a `libpng.a` symlink to `libpng16.a` (libpng's CMakeLists
# line ~1075). On Windows the static archive is `libpng16_static.lib`.
add_third_party(
	NAME libpng
	SOURCE_DIR "${OMEGACOMMON_THIRD_PARTY_SRC_DIR}/libpng/code"
	BINARY_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/temp/libpng"
	INSTALL_DIR "${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/libpng"
	CMAKE_BUILD_ARGS -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME} -DCMAKE_PREFIX_PATH=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/zlib -DPNG_SHARED=OFF -DPNG_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_C_FLAGS:LIST=-DPNG_ARM_NEON_OPT=0;-DPNG_ARM_NEON_IMPLEMENTATION=0
	EXPORT_STATIC_LIBS "png:lib/libpng.a:lib/libpng16_static.lib"
	EXPORT_INCLUDE_DIRS "include"
	DEPS zlib)

set_property(TARGET png APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_OMEGACOMMON_ZLIB_INCLUDE_DIRS}")

# --- libjpeg-turbo (static) ---
# libjpeg-turbo's static target is `turbojpeg-static`. On non-MSVC its
# OUTPUT_NAME is overridden to `turbojpeg`, so the file lands at
# lib/libturbojpeg.a. On MSVC there is no rename, so the file lands at
# lib/turbojpeg-static.lib. ENABLE_SHARED=OFF skips the shared build.
add_third_party(
	NAME libjpeg-turbo
	SOURCE_DIR "${OMEGACOMMON_THIRD_PARTY_SRC_DIR}/libjpeg-turbo/code"
	BINARY_DIR ${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/temp/libjpeg-turbo
	INSTALL_DIR ${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/libjpeg-turbo
	CMAKE_BUILD_ARGS -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME} -DCMAKE_PREFIX_PATH=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/zlib -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_PIC=ON
	EXPORT_STATIC_LIBS "turbojpeg:lib/libturbojpeg.a:lib/turbojpeg-static.lib"
	EXPORT_INCLUDE_DIRS "include"
	DEPS zlib)

# --- libtiff (static) ---
# libtiff respects BUILD_SHARED_LIBS=OFF natively. Static install path is
# lib/libtiff.a on UNIX, lib/tiff.lib on Windows.
add_third_party(
	NAME libtiff
	SOURCE_DIR ${OMEGACOMMON_THIRD_PARTY_SRC_DIR}/libtiff/code
	BINARY_DIR ${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/temp/libtiff
	INSTALL_DIR ${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/libtiff
	CMAKE_BUILD_ARGS
	-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
	-DCMAKE_PREFIX_PATH=${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/zlib;${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/libjpeg-turbo
	-DBUILD_SHARED_LIBS=OFF
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-Djpeg=OFF
	-Dold-jpeg=OFF
	-Dwebp=OFF
	-Dlzma=OFF
	-Dzstd=OFF
	-Djbig=OFF
	-Dlerc=OFF
	-Dlibdeflate=OFF
	-DCMAKE_DISABLE_FIND_PACKAGE_LibLZMA=ON
	-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON
	-DCMAKE_DISABLE_FIND_PACKAGE_JBIG=ON
	-DCMAKE_DISABLE_FIND_PACKAGE_LERC=ON
	-DCMAKE_DISABLE_FIND_PACKAGE_Deflate=ON
	-DCMAKE_DISABLE_FIND_PACKAGE_WebP=ON
	-Dtiff-tools=OFF
	-Dtiff-tests=OFF
	-Dtiff-contrib=OFF
	-Dtiff-docs=OFF
	EXPORT_STATIC_LIBS "tiff:lib/libtiff.a:lib/tiff.lib"
	EXPORT_INCLUDE_DIRS "include"
	DEPS zlib libjpeg-turbo)

set_property(TARGET tiff APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_OMEGACOMMON_ZLIB_INCLUDE_DIRS}")

# Aggregator so IDEs can build all common-owned third-party deps in one shot.
# wtk's "OmegaWTK" framework target add_dependencies on this so a fresh
# wtk configure still builds icu/zlib/libpng/libjpeg-turbo/libtiff.
add_custom_target(OmegaCommonThirdParty)
add_dependencies(OmegaCommonThirdParty icu zlib libpng libjpeg-turbo libtiff)

# === End image-codec + ICU third-party deps ===

add_omega_graphics_module("OmegaCommon" SHARED
	SOURCES ${COMMON_SRCS}
	HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
	EMBEDDED_LIBS ${ICU_RUNTIME})

# On macOS, the ICU dylibs ship with default install names. Force the
# install-name / rpath fixups to complete before OmegaCommon's POST_BUILD
# embed copies them to ${CMAKE_BINARY_DIR}/lib/, otherwise the embedded
# copies retain the original paths and consumers fail to find them at
# load time. Mirrors the OmegaWTK_ThirdPartyInstallNames aggregator that
# used to live in wtk/CMakeLists.txt before ICU embedding moved here.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
	add_dependencies(OmegaCommon
		libicuuc.dylib_install_name
		libicudata.dylib_install_name
		libicui18n.dylib_install_name
		libicuuc.dylib_libicudata.${ICU_VERSION}.dylib_reset_dependent_name
		libicui18n.dylib_libicudata.${ICU_VERSION}.dylib_reset_dependent_name
		libicui18n.dylib_libicuuc.${ICU_VERSION}.dylib_reset_dependent_name
		libicuuc.dylib__loader_path_add_rpath
		libicui18n.dylib__loader_path_add_rpath)
endif()
if(DEFINED OMEGACOMMON_RAPIDJSON_ROOT)
	set(OMEGACOMMON_RAPIDJSON_INCLUDE_DIR "${OMEGACOMMON_RAPIDJSON_ROOT}/include")
else()
	set(OMEGACOMMON_RAPIDJSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/rapidjson/include")
endif()
target_include_directories("OmegaCommon" PRIVATE "${OMEGACOMMON_RAPIDJSON_INCLUDE_DIR}")

target_link_libraries("OmegaCommon" PRIVATE pcre2-8 ssl crypto icuuc icudata icui18n png turbojpeg tiff z)
# UniString.cpp uses ICU's u_strFromUTF8 / u_strFromUTF32; the public unicode.h
# only exposes std types, so ICU stays PRIVATE. Same shape for the image deps:
# img.h doesn't leak libpng/libjpeg-turbo/libtiff/zlib types — they're impl
# detail of OmegaCommon::Img — and Phase 1b made them static archives so they
# bake into libOmegaCommon at link time.
add_dependencies("OmegaCommon" icu libpng libjpeg-turbo libtiff zlib)
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
	# ICU DLLs are now staged into ${CMAKE_BINARY_DIR}/bin/ via the
	# add_omega_graphics_module(SHARED EMBEDDED_LIBS ...) handling above.
	# (libpng/libjpeg-turbo/libtiff/zlib are static archives folded into
	# libOmegaCommon directly per Phase 1b — no runtime copy needed.)
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

# Tests run target binaries; skip them when cross-compiling.
if(NOT CMAKE_CROSSCOMPILING)
	add_subdirectory(assetc/tests)
endif()

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
