# cmake_minimum_required(VERSION 3.11)

set(OMEGAWTK_VERSION "0.8")
option(OMEGAWTK_SKIP_THIRD_PARTY_BUILD "Use prebuilt third-party artifacts and skip building ExternalProject dependencies." OFF)

set(OMEGAWTK_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})


omega_graphics_project(OmegaWTK VERSION ${OMEGAWTK_VERSION} LANGUAGES C CXX)

# if(APPLE AND NOT TARGET_METAL)
# 	enable_language(OBJCXX)
# endif()
if(IOS OR ANDROID)
	set(OMEGAWTK_MOBILE TRUE)
else()
	set(OMEGAWTK_MOBILE FALSE)
endif()

if(WIN32)
	message("OmegaWTK: TARGET_WIN32 = TRUE")
	set(TARGET_WIN32 TRUE)
	add_compile_definitions(TARGET_WIN32 TARGET_DIRECTX NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE AND NOT IOS)
	message("OmegaWTK: TARGET_MACOS = TRUE")
	set(TARGET_MACOS TRUE)
	add_compile_definitions(TARGET_MACOS TARGET_METAL)
elseif(IOS)
	message("OmegaWTK: TARGET_IOS = TRUE")
	set(TARGET_IOS TRUE)
	add_compile_definitions(TARGET_IOS TARGET_MOBILE TARGET_METAL)
	set(OMEGAWTK_MOBILE TRUE)
elseif(ANDROID)
	message("OmegaWTK: TARGET_ANDROID = TRUE")
	set(TARGET_ANDROID TRUE)
	add_compile_definitions(TARGET_ANDROID TARGET_MOBILE TARGET_VULKAN)
	set(OMEGAWTK_MOBILE TRUE)
elseif(UNIX AND NOT APPLE)
	message("OmegaWTK: TARGET_LINUX = TRUE")
	set(TARGET_LINUX TRUE)
	add_compile_definitions(TARGET_GTK TARGET_VULKAN)

	find_package(PkgConfig REQUIRED)

	pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
	pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
	pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
	pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libavdevice)
	# libnotify is optional: GTKNote falls back to no-op if not present.
	pkg_check_modules(LIBNOTIFY libnotify)

	pkg_check_modules(PKG_WAYLAND wayland-client wayland-server wayland-egl wayland-cursor)
	set(OMEGA_LINUX_VK_BACKEND "AUTO" CACHE STRING "Linux Vulkan native backend selection (AUTO, WAYLAND, X11)")
	set_property(CACHE OMEGA_LINUX_VK_BACKEND PROPERTY STRINGS AUTO WAYLAND X11)
	string(TOUPPER "${OMEGA_LINUX_VK_BACKEND}" OMEGA_LINUX_VK_BACKEND_UPPER)
	set(OMEGA_LINUX_USE_WAYLAND FALSE)

	if(OMEGA_LINUX_VK_BACKEND_UPPER STREQUAL "WAYLAND")
		if(NOT PKG_WAYLAND_FOUND)
			message(FATAL_ERROR "OMEGA_LINUX_VK_BACKEND=WAYLAND requested, but Wayland development packages were not found.")
		endif()
		set(OMEGA_LINUX_USE_WAYLAND TRUE)
	elseif(OMEGA_LINUX_VK_BACKEND_UPPER STREQUAL "X11")
		set(OMEGA_LINUX_USE_WAYLAND FALSE)
	elseif(OMEGA_LINUX_VK_BACKEND_UPPER STREQUAL "AUTO")
		if(PKG_WAYLAND_FOUND AND DEFINED ENV{XDG_SESSION_TYPE} AND "$ENV{XDG_SESSION_TYPE}" STREQUAL "wayland")
			set(OMEGA_LINUX_USE_WAYLAND TRUE)
		endif()
	else()
		message(FATAL_ERROR "Unsupported OMEGA_LINUX_VK_BACKEND value: ${OMEGA_LINUX_VK_BACKEND}. Use AUTO, WAYLAND, or X11.")
	endif()

	if(OMEGA_LINUX_USE_WAYLAND)
		message("OmegaWTK: Linux native backend = Wayland")
		add_compile_definitions("WTK_NATIVE_WAYLAND" "VULKAN_TARGET_WAYLAND")
	else()
		message("OmegaWTK: Linux native backend = X11")
		add_compile_definitions("WTK_NATIVE_X11" "VULKAN_TARGET_X11")
	endif()

endif()

if(${OMEGAWTK_MOBILE})
	add_compile_definitions(OMEGAWTK_MOBILE_API)
endif()

# Third Party Modules
#
# icu, zlib, libpng, libjpeg-turbo, libtiff moved to common/CMakeLists.txt
# in Phase 1a-1b of the Common-ImgCodec-Unicode-Refactor-Plan. The imported
# targets (icuuc, icudata, icui18n, png, turbojpeg, tiff, z) live there.
# Image libs (png/turbojpeg/tiff/z) are now STATIC archives folded into
# libOmegaCommon at link time — no separate dylib / DLL ships, so wtk's
# framework EMBEDDED_LIBS no longer references them. ICU stays SHARED and
# is still embedded by the WTK framework via ${ICU_EXPORT} / ${ICU_DLLS}
# CACHE INTERNAL variables defined in common; on Win32 OmegaCommon also
# stages those DLLs into ${CMAKE_BINARY_DIR}/bin/ post-build for
# omega_stage_runtime_dlls() consumers.
# wtk owns only libxml2 and ffmpeg now.

set(THIRD_PARTY_SRC_DIR ${OMEGAWTK_SOURCE_DIR}/deps)
set(THIRD_PARTY_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps)

# libxml2 needs zlib found at configure time and ICU's headers on its
# include path. Both live in OmegaCommon's deps install root now.
set(PREFIX_PATH ${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/zlib)

add_third_party(
	NAME libxml2
	SOURCE_DIR "${THIRD_PARTY_SRC_DIR}/libxml2/code"
	BINARY_DIR "${THIRD_PARTY_OUTPUT_DIR}/temp/libxml2"
	INSTALL_DIR "${THIRD_PARTY_OUTPUT_DIR}/libxml2"
	CMAKE_BUILD_ARGS -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME} -DLIBXML2_WITH_PYTHON=FALSE -DLIBXML2_WITH_ICU=FALSE -DLIBXML2_WITH_ICONV=FALSE -DLIBXML2_WITH_LZMA=FALSE -DCMAKE_PREFIX_PATH=${PREFIX_PATH} -DCMAKE_C_FLAGS="-I${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}/icu/include"
	EXPORT_SHARED_LIBS "xml2:lib/libxml2${CMAKE_SHARED_LIBRARY_SUFFIX}:lib/libxml2.lib"
	EXPORT_INCLUDE_DIRS "include/libxml2"
	DEPS icu)
	if(TARGET_WIN32)
		set(LIBXML2_EXPORT "${THIRD_PARTY_OUTPUT_DIR}/libxml2/lib/libxml2.lib")

		set(LIBXML2_DLL "${THIRD_PARTY_OUTPUT_DIR}/libxml2/bin/libxml2.dll")
	elseif(TARGET_MACOS)
		set(LIBXML2_EXPORT "${THIRD_PARTY_OUTPUT_DIR}/libxml2/lib/libxml2.dylib")
		set_library_install_name(${LIBXML2_EXPORT} "@rpath/libxml2.dylib" AFTER libxml2)
	elseif(TARGET_LINUX)
		set(LIBXML2_EXPORT "xml2")
	endif()

	# Per-platform runtime artifact for add_omega_graphics_module's
	# EMBEDDED_LIBS handling. Mirrors ICU_RUNTIME in common/CMakeLists.txt.
	# On Linux the EXPORT is the imported-target name "xml2", which is not
	# a path that can be embedded — leave LIBXML2_RUNTIME unset there and
	# rely on system or build-dir rpath resolution at run time.
	if(TARGET_WIN32)
		set(LIBXML2_RUNTIME "${LIBXML2_DLL}")
	elseif(TARGET_MACOS)
		set(LIBXML2_RUNTIME "${LIBXML2_EXPORT}")
	endif()

# For Building ThirdParty, so one can edit code through IDE with all code symbols.
# wtk owns only libxml2 here. icu/zlib/libpng/libjpeg-turbo/libtiff are
# aggregated under common's "OmegaCommonThirdParty" target; the OmegaWTK
# framework adds a dependency on it below so a clean wtk configure still
# builds them.

add_custom_target(ThirdParty)
add_dependencies(ThirdParty libxml2)
# ─── Gather submodule sources ───

#OmegaWTK Core SubModule

file(GLOB CORE_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Core/*.cpp")
source_group(OmegaWTKCore ${CORE_SRCS})

if(NOT ${OMEGAWTK_MOBILE})

#OmegaWTK Native SubModule

file(GLOB NATIVE_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Native/*.cpp")

if(TARGET_WIN32)
	file(GLOB WIN_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Native/win/*.cpp")
	set(NATIVE_SRCS ${NATIVE_SRCS} ${WIN_SRCS})

elseif(TARGET_MACOS)
	file(GLOB MACOS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Native/macos/*.mm")
	set(NATIVE_SRCS ${NATIVE_SRCS} ${MACOS_SRCS})
elseif(TARGET_LINUX)
	file(GLOB GTK_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Native/gtk/*.cpp")
	set(NATIVE_SRCS ${NATIVE_SRCS} ${GTK_SRCS})
endif()

source_group(OmegaWTKNative ${NATIVE_SRCS})
set(NATIVE_PRIVATE_INCLUDE_DIR "${OMEGAWTK_SOURCE_DIR}/src/Native/private_include")

#OmegaWTK Media SubModule

file(GLOB MEDIA_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/*.cpp")
source_group(OmegaWTKMedia ${MEDIA_SRCS})
if(TARGET_WIN32)
	file(GLOB WIN_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/wmf/*.cpp")
	set(MEDIA_SRCS ${MEDIA_SRCS} ${WIN_SRCS})

elseif(TARGET_MACOS)
	file(GLOB MACOS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/avf/*.mm")
	set(MEDIA_SRCS ${MEDIA_SRCS} ${MACOS_SRCS})
else()
	file(GLOB FFMPEG_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/ffmpeg/*.cpp")
	set(MEDIA_SRCS ${MEDIA_SRCS} ${FFMPEG_SRCS})
endif()
# Mobile Sources
else()

	file(GLOB NATIVE_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Native/*.cpp")
	if(TARGET_ANDROID)
		file(GLOB ANDROID_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Native/android/*.cpp")
		set(NATIVE_SRCS ${NATIVE_SRCS} ${ANDROID_SRCS})
	else()
		file(GLOB IOS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Native/ios/*.mm")
		set(NATIVE_SRCS ${NATIVE_SRCS} ${IOS_SRCS})
	endif()
	source_group(OmegaWTKNative ${NATIVE_SRCS})
	set(NATIVE_PRIVATE_INCLUDE_DIR "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Native/private_include")

	#OmegaWTK Media SubModule

	file(GLOB MEDIA_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/*.cpp")
	source_group(OmegaWTKMedia ${MEDIA_SRCS})
	if(TARGET_ANDROID)
		file(GLOB ANDROID_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Media/android/*.cpp")
		set(MEDIA_SRCS ${MEDIA_SRCS} ${ANDROID_SRCS})
	else()
		file(GLOB IOS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Mobile/Media/ios/*.mm")
		set(MEDIA_SRCS ${MEDIA_SRCS} ${IOS_SRCS})
	endif()
endif()

#OmegaWTK Composition SubModule

file(GLOB COMPOSITION_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Composition/*.cpp" "${OMEGAWTK_SOURCE_DIR}/src/Composition/backend/*.cpp")

if(TARGET_WIN32)
	file(GLOB WIN_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Composition/backend/dx/*.cpp")
	set(COMPOSITION_SRCS ${COMPOSITION_SRCS} ${WIN_SRCS})
elseif(TARGET_MACOS)
	file(GLOB MACOS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Composition/backend/mtl/*.mm")
	set(COMPOSITION_SRCS ${COMPOSITION_SRCS} ${MACOS_SRCS})
elseif(TARGET_LINUX)
	file(GLOB VK_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Composition/backend/vk/*.cpp")
	set(COMPOSITION_SRCS ${COMPOSITION_SRCS} ${VK_SRCS})
endif()

source_group(OmegaWTKComposition ${COMPOSITION_SRCS})

#OmegaWTK UI SubModule
file(GLOB UI_SRCS "${OMEGAWTK_SOURCE_DIR}/src/UI/*.cpp")
source_group(OmegaWTKUI ${UI_SRCS})

#OmegaWTK Widgets Submodule
file(GLOB WIDGETS_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Widgets/*.cpp")
source_group(OmegaWTKWidgets ${WIDGETS_SRCS})

# ─── Submodule static libraries ───

set(OMEGAWTK_PUBLIC_INCLUDE_DIR "${OMEGAWTK_SOURCE_DIR}/include")

add_library(OmegaWTK_Core STATIC ${CORE_SRCS})
target_include_directories(OmegaWTK_Core
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE "${OMEGAWTK_SOURCE_DIR}/tools/assetc"
	        $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_link_libraries(OmegaWTK_Core PUBLIC OmegaCommon xml2)

add_library(OmegaWTK_Native STATIC ${NATIVE_SRCS})
target_include_directories(OmegaWTK_Native
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE ${NATIVE_PRIVATE_INCLUDE_DIR})
target_link_libraries(OmegaWTK_Native PUBLIC OmegaCommon)
if(TARGET_LINUX)
	target_include_directories(OmegaWTK_Native PUBLIC ${GTK3_INCLUDE_DIRS})
	target_compile_options(OmegaWTK_Native PUBLIC ${GTK3_CFLAGS_OTHER})
	target_link_directories(OmegaWTK_Native PUBLIC ${GTK3_LIBRARY_DIRS})
	target_link_libraries(OmegaWTK_Native PUBLIC ${GTK3_LIBRARIES})
	if(LIBNOTIFY_FOUND)
		target_include_directories(OmegaWTK_Native PRIVATE ${LIBNOTIFY_INCLUDE_DIRS})
		target_compile_options(OmegaWTK_Native PRIVATE ${LIBNOTIFY_CFLAGS_OTHER})
		target_link_directories(OmegaWTK_Native PUBLIC ${LIBNOTIFY_LIBRARY_DIRS})
		target_link_libraries(OmegaWTK_Native PUBLIC ${LIBNOTIFY_LIBRARIES})
		target_compile_definitions(OmegaWTK_Native PRIVATE OMEGAWTK_HAS_LIBNOTIFY)
	else()
		message(WARNING "OmegaWTK: libnotify not found; GTKNote will be a no-op.")
	endif()
endif()

add_library(OmegaWTK_Media STATIC ${MEDIA_SRCS})
target_include_directories(OmegaWTK_Media
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>)
target_link_libraries(OmegaWTK_Media PUBLIC OmegaCommon)
if(TARGET_LINUX)
	target_include_directories(OmegaWTK_Media PRIVATE ${FFMPEG_INCLUDE_DIRS})
	target_compile_options(OmegaWTK_Media PRIVATE ${FFMPEG_CFLAGS_OTHER})
	target_link_directories(OmegaWTK_Media PUBLIC ${FFMPEG_LIBRARY_DIRS})
	target_link_libraries(OmegaWTK_Media PUBLIC ${FFMPEG_LIBRARIES})
endif()

add_library(OmegaWTK_Composition STATIC ${COMPOSITION_SRCS})
target_include_directories(OmegaWTK_Composition
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
	        ${NATIVE_PRIVATE_INCLUDE_DIR})
target_link_libraries(OmegaWTK_Composition PUBLIC OmegaCommon PRIVATE OmegaGTE)
if(TARGET_LINUX)
	# vk backend pulls in GTKItem.h (GTK) plus pangocairo/fontconfig for text layout.
	target_include_directories(OmegaWTK_Composition
		PRIVATE ${GTK3_INCLUDE_DIRS} ${PANGOCAIRO_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
	target_compile_options(OmegaWTK_Composition
		PRIVATE ${GTK3_CFLAGS_OTHER} ${PANGOCAIRO_CFLAGS_OTHER} ${FONTCONFIG_CFLAGS_OTHER})
	target_link_directories(OmegaWTK_Composition
		PUBLIC ${GTK3_LIBRARY_DIRS} ${PANGOCAIRO_LIBRARY_DIRS} ${FONTCONFIG_LIBRARY_DIRS})
	target_link_libraries(OmegaWTK_Composition
		PUBLIC ${GTK3_LIBRARIES} ${PANGOCAIRO_LIBRARIES} ${FONTCONFIG_LIBRARIES})
endif()

add_library(OmegaWTK_UI STATIC ${UI_SRCS})
target_include_directories(OmegaWTK_UI
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_link_libraries(OmegaWTK_UI PUBLIC OmegaCommon)

add_library(OmegaWTK_Widgets STATIC ${WIDGETS_SRCS})
target_include_directories(OmegaWTK_Widgets
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_link_libraries(OmegaWTK_Widgets PUBLIC OmegaCommon)

set(OMEGAWTK_SUBMODULE_LIBS OmegaWTK_Core OmegaWTK_Native OmegaWTK_Media OmegaWTK_Composition OmegaWTK_UI OmegaWTK_Widgets)

# Submodule archives get whole-archive'd into the OmegaWTK shared library, so
# every object file must be position-independent.
set_target_properties(${OMEGAWTK_SUBMODULE_LIBS} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─── OmegaWTK framework module ───

configure_file("${OMEGAWTK_SOURCE_DIR}/src/Info.plist.in" ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

set(OMEGAWTK_MODULE_DEPS OmegaGTE icu zlib libpng libjpeg-turbo libtiff libxml2)
if(OMEGAWTK_SKIP_THIRD_PARTY_BUILD)
	set(OMEGAWTK_MODULE_DEPS OmegaGTE)
endif()

# Compositor shaders are precompiled at build time by `omegaslc` (via the
# `add_omegasl_lib` helper from `gte/OmegaGTE.cmake`) into a host-native
# `.omegasllib`. The library file is deployed alongside the executable and
# loaded once at engine init by `OmegaGraphicsEngine::loadShaderLibrary`.
# Building the lib is gated on the `omegaslc` tool, so the dependency edge
# `OmegaWTK_Composition -> OmegaWTKCompositorShaderLib -> omegaslc` keeps
# the order correct on a clean configure.
set(OMEGAWTK_COMPOSITOR_SHADER_SRC "${OMEGAWTK_SOURCE_DIR}/src/Composition/backend/shaders/compositor.omegasl" CACHE INTERNAL "")
set(OMEGAWTK_COMPOSITOR_SHADER_LIB "${CMAKE_CURRENT_BINARY_DIR}/compositor.omegasllib" CACHE INTERNAL "")
add_omegasl_lib(OmegaWTKCompositorShaderLib
                "${OMEGAWTK_COMPOSITOR_SHADER_SRC}"
                "${OMEGAWTK_COMPOSITOR_SHADER_LIB}")
add_dependencies(OmegaWTKCompositorShaderLib omegaslc)
add_dependencies(OmegaWTK_Composition OmegaWTKCompositorShaderLib)

# Generate a stub translation unit -- all real code lives in the submodule static libs.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/OmegaWTK_framework_stub.cpp" "// OmegaWTK framework entry point -- submodules linked as static libraries\n")

add_omega_graphics_module("OmegaWTK" FRAMEWORK INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" VERSION ${OMEGAWTK_VERSION} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
DEPENDS ${OMEGAWTK_MODULE_DEPS} ${OMEGAWTK_SUBMODULE_LIBS}
EMBEDDED_LIBS ${LIBXML2_RUNTIME}
SOURCES "${CMAKE_CURRENT_BINARY_DIR}/OmegaWTK_framework_stub.cpp")
if(NOT OMEGAWTK_SKIP_THIRD_PARTY_BUILD)
	add_dependencies("OmegaWTK" ThirdParty)
	# icu/zlib/libpng/libjpeg-turbo/libtiff are owned by common as of
	# Phase 1a of the Common-ImgCodec-Unicode-Refactor-Plan.
	add_dependencies("OmegaWTK" OmegaCommonThirdParty)
endif()
# Link submodule static libraries into the framework.
# Use whole-archive flags to ensure all symbols are exported from the shared library.
if(APPLE)
	foreach(_submod ${OMEGAWTK_SUBMODULE_LIBS})
		target_link_libraries("OmegaWTK" PRIVATE "-Wl,-force_load,$<TARGET_FILE:${_submod}>")
	endforeach()
elseif(WIN32)
	foreach(_submod ${OMEGAWTK_SUBMODULE_LIBS})
		target_link_options("OmegaWTK" PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${_submod}>")
	endforeach()
else()
	target_link_libraries("OmegaWTK" PRIVATE
		"-Wl,--whole-archive" ${OMEGAWTK_SUBMODULE_LIBS} "-Wl,--no-whole-archive")
endif()

if(TARGET_ANDROID)
	add_library(app-glue STATIC
		${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
	target_link_libraries("OmegaWTK" PRIVATE "app-glue")
	target_include_directories("OmegaWTK" PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
endif()
if(TARGET_MACOS)
	# Only libxml2 ships as a WTK-owned embedded dylib. ICU's install-name
	# fixups belong to OmegaCommon now (it embeds them into its own SHARED
	# layout). The image libs (png/jpeg/tiff/z) are static archives folded
	# into libOmegaCommon at link time.
	add_custom_target("OmegaWTK_ThirdPartyInstallNames")
	if(LIBXML2_RUNTIME)
		get_filename_component(LIBXML2_NAME "${LIBXML2_RUNTIME}" NAME)
		add_dependencies("OmegaWTK_ThirdPartyInstallNames" ${LIBXML2_NAME}_install_name)
	endif()


	add_dependencies("OmegaWTK" OmegaGTE.framework)
	add_dependencies("OmegaWTK" "OmegaWTK_ThirdPartyInstallNames")
endif()
target_include_directories("OmegaWTK" PUBLIC $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>)
if(TARGET_MACOS)
	target_link_system_frameworks("OmegaWTK" Metal Cocoa QuartzCore AVFoundation VideoToolbox AudioToolbox CoreMedia CoreAudio UserNotifications)
	target_link_frameworks("OmegaWTK" OmegaGTE)
else()
	target_link_libraries("OmegaWTK" PRIVATE OmegaGTE)
endif()
target_link_libraries("OmegaWTK" PUBLIC "OmegaCommon" xml2)

if(TARGET_WIN32)
	target_compile_definitions("OmegaWTK" PRIVATE WINDOWS_PRIVATE)
endif()
if(TARGET_LINUX)
	# GTK/FFmpeg/Pango/Fontconfig flags are bound directly to the submodule
	# static libraries (Native, Media, Composition) above; the framework
	# inherits them transitively via PUBLIC linkage against those submodules.
endif()

include(cmake/OmegaWTKApp.cmake)

 add_subdirectory(tests)
