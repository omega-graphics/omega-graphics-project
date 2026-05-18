# cmake_minimum_required(VERSION 3.11)

set(OMEGAWTK_VERSION "0.8")
option(OMEGAWTK_SKIP_THIRD_PARTY_BUILD "Use prebuilt third-party artifacts and skip building ExternalProject dependencies." OFF)
# Tier 3 Phase 3.0 (UIView-Render-Redesign-Plan): scaffolding flag for the
# window-scoped paint path. OFF keeps every test on the per-view Canvas
# path the framework has shipped since Tier 2. ON makes the runtime knob
# AppWindow::setWindowScopedPaint() default to true. Phase 3.8 deletes the
# flag once the per-view path is gone.
option(OMEGAWTK_WINDOW_SCOPED_PAINT "Default the AppWindow window-scoped paint runtime knob to ON (Tier 3)." OFF)

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
	# Phase 6.7: outline extraction goes through PangoFc → FT_Face. The
	# pangoft2 module exposes the PangoFcFont APIs and pulls freetype as
	# an Imported transitive dep so we don't need a separate find call.
	pkg_check_modules(PANGOFT2 REQUIRED pangoft2)
	pkg_check_modules(FREETYPE2 REQUIRED freetype2)
	pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
	# FFmpeg + alsa-lib options moved to va/CMakeLists.txt as part of the
	# OmegaVA extraction (Phase 6 of Media-API-Completion-Plan).

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

# NOTE: FFmpeg + alsa-lib vendored-build blocks moved to va/CMakeLists.txt
# as part of the OmegaVA extraction (Phase 6 of Media-API-Completion-Plan).
# WTK no longer compiles any Media TUs — it consumes OmegaVA via the
# target_link_libraries on OmegaWTK_UI below.

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

# Media sources moved to OmegaVA (va/) — see Phase 6 of Media-API-Completion-Plan.

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

# OmegaWTK_Media moved to top-level OmegaVA module (see va/CMakeLists.txt
# and Phase 6 of Media-API-Completion-Plan). OmegaWTK_UI now links OmegaVA
# below to pick up VideoView's playback / capture session dependencies.

add_library(OmegaWTK_Composition STATIC ${COMPOSITION_SRCS})
target_include_directories(OmegaWTK_Composition
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
	        ${NATIVE_PRIVATE_INCLUDE_DIR})
target_link_libraries(OmegaWTK_Composition PUBLIC OmegaCommon PRIVATE OmegaGTE)

# msdfgen — Phase 6.7 MSDF glyph atlas. Core-only build (no SVG / PNG /
# OpenMP / vcpkg / standalone exe / freetype-via-msdfgen-ext). We feed
# msdfgen pre-built `Shape` objects from the per-platform outline
# extractor (PangoFc → FT_Outline_Decompose on Linux, Core Text →
# CGPathApply on macOS, IDWriteFontFace::GetGlyphRunOutline +
# IDWriteGeometrySink on Windows), so the msdfgen-ext dependency on
# freetype-inside-msdfgen is unnecessary on every platform.
if(TARGET_LINUX OR TARGET_MACOS OR TARGET_WIN32)
	set(MSDFGEN_DIR "${OMEGAWTK_SOURCE_DIR}/deps/msdfgen/code")
	if(EXISTS "${MSDFGEN_DIR}/CMakeLists.txt")
		set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
		set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
		set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
		set(MSDFGEN_USE_OPENMP       OFF CACHE BOOL "" FORCE)
		set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
		set(MSDFGEN_DISABLE_SVG      ON  CACHE BOOL "" FORCE)
		set(MSDFGEN_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
		set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
		set(MSDFGEN_DYNAMIC_RUNTIME  OFF CACHE BOOL "" FORCE)
		add_subdirectory("${MSDFGEN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/deps/msdfgen" EXCLUDE_FROM_ALL)
		# Whole-archive into the OmegaWTK shared lib means msdfgen has
		# to be PIC too.
		if(TARGET msdfgen-core)
			set_target_properties(msdfgen-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
		endif()
		target_link_libraries(OmegaWTK_Composition PRIVATE msdfgen::msdfgen)
		target_compile_definitions(OmegaWTK_Composition PRIVATE OMEGAWTK_HAVE_MSDFGEN=1)
	else()
		message(STATUS "OmegaWTK: msdfgen not present at ${MSDFGEN_DIR}; MSDF text disabled (will fall back to bitmap path).")
	endif()
endif()
if(TARGET_LINUX)
	# vk backend pulls in GTKItem.h (GTK) plus pangocairo/fontconfig for text layout.
	# Phase 6.7 adds pangoft2 + freetype2 for the PangoFc→FT_Face descent
	# used by the MSDF outline extractor.
	target_include_directories(OmegaWTK_Composition
		PRIVATE ${GTK3_INCLUDE_DIRS} ${PANGOCAIRO_INCLUDE_DIRS} ${PANGOFT2_INCLUDE_DIRS}
		        ${FREETYPE2_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
	target_compile_options(OmegaWTK_Composition
		PRIVATE ${GTK3_CFLAGS_OTHER} ${PANGOCAIRO_CFLAGS_OTHER} ${PANGOFT2_CFLAGS_OTHER}
		        ${FREETYPE2_CFLAGS_OTHER} ${FONTCONFIG_CFLAGS_OTHER})
	target_link_directories(OmegaWTK_Composition
		PUBLIC ${GTK3_LIBRARY_DIRS} ${PANGOCAIRO_LIBRARY_DIRS} ${PANGOFT2_LIBRARY_DIRS}
		       ${FREETYPE2_LIBRARY_DIRS} ${FONTCONFIG_LIBRARY_DIRS})
	target_link_libraries(OmegaWTK_Composition
		PUBLIC ${GTK3_LIBRARIES} ${PANGOCAIRO_LIBRARIES} ${PANGOFT2_LIBRARIES}
		       ${FREETYPE2_LIBRARIES} ${FONTCONFIG_LIBRARIES})
endif()

add_library(OmegaWTK_UI STATIC ${UI_SRCS})
target_include_directories(OmegaWTK_UI
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	       $<TARGET_PROPERTY:OmegaVA,INCLUDE_DIRECTORIES>
	# VideoView.cpp dtor of SharedHandle<AudioVideoProcessor> needs the
	# backend AudioVideoProcessor full type; va/src is the private home
	# for the per-backend headers (wmf/avf/ffmpeg). PRIVATE because no
	# WTK public header pulls them.
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
	        $<TARGET_PROPERTY:OmegaVA,SOURCE_DIR>/src)
target_link_libraries(OmegaWTK_UI PUBLIC OmegaCommon OmegaVA)
if(OMEGAWTK_WINDOW_SCOPED_PAINT)
	target_compile_definitions(OmegaWTK_UI PRIVATE OMEGAWTK_WINDOW_SCOPED_PAINT=1)
endif()

add_library(OmegaWTK_Widgets STATIC ${WIDGETS_SRCS})
target_include_directories(OmegaWTK_Widgets
	PUBLIC ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_link_libraries(OmegaWTK_Widgets PUBLIC OmegaCommon)

set(OMEGAWTK_SUBMODULE_LIBS OmegaWTK_Core OmegaWTK_Native OmegaWTK_Composition OmegaWTK_UI OmegaWTK_Widgets)

# Submodule archives get whole-archive'd into the OmegaWTK shared library, so
# every object file must be position-independent.
set_target_properties(${OMEGAWTK_SUBMODULE_LIBS} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─── OmegaWTK framework module ───

configure_file("${OMEGAWTK_SOURCE_DIR}/src/Info.plist.in" ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

set(OMEGAWTK_MODULE_DEPS OmegaGTE OmegaVA icu zlib libpng libjpeg-turbo libtiff libxml2)
if(OMEGAWTK_SKIP_THIRD_PARTY_BUILD)
	set(OMEGAWTK_MODULE_DEPS OmegaGTE OmegaVA)
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
	# The submodules are force-loaded by raw path ($<TARGET_FILE:>), which
	# bypasses CMake's transitive link-interface propagation — so
	# OmegaWTK_Composition's PRIVATE msdfgen dependency (Phase 6.7 MSDF
	# text) must be linked into the framework explicitly here.
	if(TARGET msdfgen::msdfgen)
		target_link_libraries("OmegaWTK" PRIVATE msdfgen::msdfgen)
	endif()
elseif(WIN32)
	foreach(_submod ${OMEGAWTK_SUBMODULE_LIBS})
		target_link_options("OmegaWTK" PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${_submod}>")
	endforeach()
	# /WHOLEARCHIVE bypasses transitive link-interface propagation the same
	# way Apple's -force_load does — so OmegaWTK_Composition's PRIVATE
	# msdfgen dependency (Phase 6.7 MSDF text) has to be linked into the
	# framework explicitly here.
	if(TARGET msdfgen::msdfgen)
		target_link_libraries("OmegaWTK" PRIVATE msdfgen::msdfgen)
	endif()
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
target_include_directories("OmegaWTK" PUBLIC
	$<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:OmegaVA,INCLUDE_DIRECTORIES>)
if(TARGET_MACOS)
	target_link_system_frameworks("OmegaWTK" Metal Cocoa QuartzCore AVFoundation VideoToolbox AudioToolbox CoreMedia CoreAudio UserNotifications)
	target_link_frameworks("OmegaWTK" OmegaGTE)
else()
	target_link_libraries("OmegaWTK" PRIVATE OmegaGTE)
endif()
target_link_libraries("OmegaWTK" PUBLIC "OmegaCommon" "OmegaVA" xml2)

if(TARGET_WIN32)
	target_compile_definitions("OmegaWTK" PRIVATE WINDOWS_PRIVATE)
endif()
if(TARGET_LINUX)
	# GTK/FFmpeg/Pango/Fontconfig flags are bound directly to the submodule
	# static libraries (Native, Media, Composition) above; the framework
	# inherits them transitively via PUBLIC linkage against those submodules.

	# Project convention: every runtime shared library lands in
	# ${CMAKE_BINARY_DIR}/lib. libOmegaWTK.so itself already gets
	# placed there by add_omega_graphics_module's SHARED branch, but
	# its FFmpeg + alsa-lib dependencies install into their own
	# ExternalProject `lib` dirs and would otherwise require the user
	# to set LD_LIBRARY_PATH. Stage them into ${CMAKE_BINARY_DIR}/lib
	# so the loader resolves them via libOmegaWTK.so's $ORIGIN rpath.
	#
	# On macOS / Windows the framework bundle / DLL-side-by-side
	# mechanisms handle this — see add_app_bundle and
	# omega_stage_runtime_dlls respectively — so this block is
	# Linux-only.
	#
	# $ORIGIN rpath on libOmegaWTK: the EMBEDDED_LIBS branch in
	# add_omega_graphics_module appends $ORIGIN only when the
	# EMBEDDED_LIBS list is non-empty; on Linux LIBXML2_RUNTIME is
	# unset (libxml2 is the system / vendored xml2 imported target),
	# so EMBEDDED_LIBS is empty and the rpath wasn't appended.
	# Append it explicitly here so libavcodec.so.61 / libasound.so.2
	# resolve from the staged ${CMAKE_BINARY_DIR}/lib alongside
	# libOmegaWTK.so itself.
	set_property(TARGET "OmegaWTK" APPEND PROPERTY BUILD_RPATH   "$ORIGIN")
	set_property(TARGET "OmegaWTK" APPEND PROPERTY INSTALL_RPATH "$ORIGIN")

	# Use DT_RPATH (old-style dynamic tag) instead of DT_RUNPATH on
	# libOmegaWTK.so. The FFmpeg shared libs (libavdevice, libavfilter,
	# ...) are installed with no rpath of their own and reference each
	# other by SONAME. When loaded as a dependency of libOmegaWTK, they
	# need a search path for *their* DT_NEEDED resolutions —
	# specifically `libavdevice.so.61` → `libavfilter.so.10`. DT_RUNPATH
	# (the modern default emitted by --enable-new-dtags) applies only
	# to the object that carries it, so libOmegaWTK's RUNPATH does NOT
	# get consulted when libavdevice looks up libavfilter; the load
	# fails even though libavfilter.so.10 is staged in
	# ${CMAKE_BINARY_DIR}/lib alongside libOmegaWTK.so. DT_RPATH IS
	# transitive across the dependency chain — set it explicitly via
	# --disable-new-dtags so libOmegaWTK's rpath covers every dynamic
	# object loaded through it (the staged FFmpeg + alsa-lib chain
	# included). This is the same pattern Linux Qt / Chromium / Firefox
	# use when shipping co-located third-party shared libs.
	target_link_options("OmegaWTK" PRIVATE "-Wl,--disable-new-dtags")

	if(OMEGAWTK_USE_VENDORED_FFMPEG)
		add_custom_command(TARGET "OmegaWTK" POST_BUILD
			COMMAND "${CMAKE_COMMAND}"
				"-DSRC_DIR=${FFMPEG_INSTALL_DIR}/lib"
				"-DDST_DIR=${CMAKE_BINARY_DIR}/lib"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeSharedLibs.cmake"
			COMMENT "Staging FFmpeg shared libs into ${CMAKE_BINARY_DIR}/lib"
			VERBATIM)
		add_dependencies("OmegaWTK" ffmpeg)
	endif()

	if(OMEGAWTK_USE_VENDORED_ALSA)
		add_custom_command(TARGET "OmegaWTK" POST_BUILD
			COMMAND "${CMAKE_COMMAND}"
				"-DSRC_DIR=${ALSA_INSTALL_DIR}/lib"
				"-DDST_DIR=${CMAKE_BINARY_DIR}/lib"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeSharedLibs.cmake"
			COMMENT "Staging alsa-lib shared libs into ${CMAKE_BINARY_DIR}/lib"
			VERBATIM)
		add_dependencies("OmegaWTK" alsa-lib)
	endif()
endif()

include(cmake/OmegaWTKApp.cmake)

 add_subdirectory(tests)
