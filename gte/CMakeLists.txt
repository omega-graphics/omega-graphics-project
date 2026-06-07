# cmake_minimum_required(VERSION 3.9)

set(OMEGAGTE_VERSION "0.6")

omega_graphics_project(OmegaGTE VERSION ${OMEGAGTE_VERSION} LANGUAGES CXX)


include(./OmegaGTE.cmake)

file(GLOB MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB COMMON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp")

# MeshParser is now compiled on every backend, Metal included: it grew a
# ufbx-backed FBX parser, and FBX is a format Model I/O cannot load, so the
# Metal asset loader routes .fbx through the shared parser. OBJ / glTF on Metal
# still go through Model I/O / MTKMesh (strictly more capable there).

set(PUBLIC_DEFS)

if(WIN32)

	set(TARGET_DIRECTX TRUE)

	set(PUBLIC_DEFS "TARGET_DIRECTX")

	# Suppress the windows.h min/max macros so they don't clobber std::min/std::max
	# (GE.h pulls in <windows.h> on this target). PUBLIC so consumers inherit it too.
	list(APPEND PUBLIC_DEFS "NOMINMAX")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/*.cpp")

elseif(APPLE)

	set(TARGET_METAL TRUE)

	set(PUBLIC_DEFS "TARGET_METAL")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/metal/*.mm")

	# enable_language(OBJCXX)

else()
	set(TARGET_VULKAN TRUE)

	find_package(PkgConfig)

	pkg_check_modules(PKG_WAYLAND wayland-client wayland-server wayland-egl wayland-cursor)
	set(OMEGA_LINUX_VK_BACKEND "AUTO" CACHE STRING "Linux Vulkan native backend selection (AUTO, WAYLAND, X11)")
	set_property(CACHE OMEGA_LINUX_VK_BACKEND PROPERTY STRINGS AUTO WAYLAND X11)
	string(TOUPPER "${OMEGA_LINUX_VK_BACKEND}" OMEGA_LINUX_VK_BACKEND_UPPER)
	set(OMEGA_LINUX_USE_WAYLAND FALSE)

	set(PUBLIC_DEFS "TARGET_VULKAN")

	if(ANDROID)
		list(APPEND PUBLIC_DEFS "VULKAN_TARGET_ANDROID")
	elseif(OMEGA_LINUX_VK_BACKEND_UPPER STREQUAL "WAYLAND")
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
		message("OmegaGTE: Linux Vulkan backend = Wayland")
		list(APPEND PUBLIC_DEFS "VULKAN_TARGET_WAYLAND")
	elseif(NOT ANDROID)
		message("OmegaGTE: Linux Vulkan backend = X11")
		list(APPEND PUBLIC_DEFS "VULKAN_TARGET_X11")
	endif()

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/*.cpp")
endif()

set(MAIN_SRCS ${MAIN_SRCS} ${COMMON_SRCS} ${RHI_SRCS})

file(GLOB OMEGASL_SRCS "omegasl/src/*.h" "omegasl/src/*.cpp")
if(TARGET_METAL)
	set(OMEGASL_SRCS ${OMEGASL_SRCS} "omegasl/src/MetalShaderCompile.mm")
endif()

# omegaslc: the OmegaSL shader compiler. Declared via add_omega_graphics_tool,
# which on cross-compile builds it as a host superbuild automatically.
add_omega_graphics_tool("omegaslc" SOURCES ${OMEGASL_SRCS} LIBS "OmegaCommon")
target_include_directories("omegaslc" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions("omegaslc" PUBLIC ${PUBLIC_DEFS})

# omegaslc's host-side dependencies (shaderc, glslc on Vulkan; Metal/Foundation
# on Apple) only matter when we're building it inline — i.e. native build, or
# the host-tools superbuild itself. In a cross-compile parent build the tool
# is a shim whose binary is replaced post-build by the host output, so we don't
# wire these deps there.
if(OMEGA_HOST_TOOLS_ONLY OR NOT CMAKE_CROSSCOMPILING)
	if(TARGET_VULKAN)
		set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
		set(VULKAN_SDK_DIR ${THIRD_PARTY_DIR}/vulkan_sdk/1.3.283.0)
		set(VULKAN_INCLUDE_DIRECTORY "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/include")
		set(VULKAN_SDK_GLSLC "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/bin/glslc")

		file(COPY "${VULKAN_SDK_GLSLC}" DESTINATION "${CMAKE_BINARY_DIR}/bin"
			 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

		target_compile_definitions("omegaslc" PRIVATE "OMEGASL_DEFAULT_GLSLC=\"${CMAKE_BINARY_DIR}/bin/glslc\"")
		install(PROGRAMS "${VULKAN_SDK_GLSLC}" DESTINATION bin)

		target_link_libraries("omegaslc" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib/libshaderc_shared.so.1")
		target_include_directories("omegaslc" PRIVATE ${VULKAN_INCLUDE_DIRECTORY})
	endif()

	if(TARGET_METAL)
		target_link_system_frameworks("omegaslc" Metal Foundation)
	endif()
endif()

# Host-tools-only mode: stop here. The OmegaGTE library itself is not built
# in this mode — its consumer (the cross-compile parent build) is what owns
# OmegaGTE for the device.
if(OMEGA_HOST_TOOLS_ONLY)
	return()
endif()

# ---------------------------------------------------------------------------
# OmegaGTE library (target platform).
# ---------------------------------------------------------------------------

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

add_omega_graphics_module("OmegaGTE" FRAMEWORK INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" VERSION ${OMEGAGTE_VERSION} HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" DEPENDS "OmegaCommon" SOURCES ${MAIN_SRCS})
target_compile_definitions("OmegaGTE" PRIVATE OMEGAGTE__BUILD__)
target_compile_definitions("OmegaGTE" PUBLIC ${PUBLIC_DEFS})

# OMEGAGTE_DEBUG: build-default for GTEInitOptions::DebugLayer::Default.
# Debug / RelWithDebInfo configs default the debug layer ON (D3D12 debug
# layer + Vulkan validation + verbose DEBUG_STREAM logging); Release /
# MinSizeRel default it OFF. Callers can still override per-Init via
# GTEInitOptions::debugLayer = Enabled | Disabled.
set(OMEGAGTE_DEBUG_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR NOT CMAKE_BUILD_TYPE)
    set(OMEGAGTE_DEBUG_DEFAULT ON)
endif()
option(OMEGAGTE_DEBUG "Enable the OmegaGTE debug layer by default" ${OMEGAGTE_DEBUG_DEFAULT})
if(OMEGAGTE_DEBUG)
    target_compile_definitions("OmegaGTE" PUBLIC OMEGAGTE_DEBUG)
endif()
target_link_libraries("OmegaGTE" PUBLIC "OmegaCommon")

# Shared mesh-parser dependencies (src/common/MeshParser.cpp) — compiled on
# every backend now that the parser handles FBX (which Metal's Model I/O can't
# load). cgltf is header-only; ufbx ships ufbx.h + ufbx.c, so its
# implementation TU is added to the sources here. The per-backend blocks below
# add cgltf again for Vulkan / D3D12 — harmless, CMake de-dups include dirs.
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
target_include_directories("OmegaGTE" PRIVATE
	"${THIRD_PARTY_DIR}/cgltf"
	"${THIRD_PARTY_DIR}/ufbx")
target_sources("OmegaGTE" PRIVATE "${THIRD_PARTY_DIR}/ufbx/ufbx.c")

# OmegaGTE embeds the OmegaSL parser/codegen sources for runtime shader
# compile paths. On Android, GLSLTarget.cpp pulls libshaderc which the NDK
# does not ship, so we exclude OMEGASL_SRCS there. iOS keeps them — it uses
# the Metal path (MetalShaderCompile.mm) which has no shaderc dependency.
if(NOT ANDROID)
	target_sources("OmegaGTE" PRIVATE ${OMEGASL_SRCS})
endif()

if(TARGET_VULKAN)
	set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
	set(VMA_INCLUDE_DIRECTORY "${THIRD_PARTY_DIR}/vulkan-memory-allocator/include")
	set(GLM_INCLUDE_DIRECTORY "${THIRD_PARTY_DIR}/glm")

	if(ANDROID)
		# NDK sysroot supplies <vulkan/vulkan.h> and libvulkan; no LunarG
		# SDK fetch and no shaderc/glslc on device.
		target_include_directories("OmegaGTE" PRIVATE ${VMA_INCLUDE_DIRECTORY} ${GLM_INCLUDE_DIRECTORY})
		target_link_libraries("OmegaGTE" PRIVATE vulkan)
	else()
		set(VULKAN_SDK_DIR ${THIRD_PARTY_DIR}/vulkan_sdk/1.3.283.0)
		set(VULKAN_INCLUDE_DIRECTORY "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/include")
		target_link_directories("OmegaGTE" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib")
		target_include_directories("OmegaGTE" PRIVATE ${VULKAN_INCLUDE_DIRECTORY} ${VMA_INCLUDE_DIRECTORY} ${GLM_INCLUDE_DIRECTORY})
		target_link_libraries("OmegaGTE" PRIVATE libvulkan.so.1 libshaderc_shared.so)
	endif()

	# cgltf is single-header; the implementation TU lives in
	# src/common/MeshParser.cpp behind a CGLTF_IMPLEMENTATION define.
	# OBJ is parsed inline in MeshParser.cpp (no external dep).
	# Vulkan asset loaders consume the shared parser; image decoding
	# for GEVulkanTextureAsset goes through OmegaCommon::Img (libpng /
	# turbojpeg / libtiff already linked transitively via OmegaCommon),
	# so no extra image-codec deps are required here.
	target_include_directories("OmegaGTE" PRIVATE "${THIRD_PARTY_DIR}/cgltf")
endif()
if(TARGET_DIRECTX)
	set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
	set(D3D12MA_DIR "${THIRD_PARTY_DIR}/D3D12MemoryAllocator")
	set(D3D12MA_INCLUDE_DIRECTORY "${D3D12MA_DIR}/include")
	set(D3D12MA_SOURCE "${D3D12MA_DIR}/src/D3D12MemAlloc.cpp")

	target_include_directories("OmegaGTE" PRIVATE ${D3D12MA_INCLUDE_DIRECTORY})
	target_sources("OmegaGTE" PRIVATE ${D3D12MA_SOURCE})

	# DirectXTex / DirectXMesh — Phase 3.4 asset loaders. Sub-projects
	# expose `option()`s for samples / DX11 / etc.; we force them off
	# before add_subdirectory so DirectXTex_Desktop_2026.slnx-style
	# extras (DDSView, fxc dependency) don't enter the build graph.
	set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
	set(BUILD_DX11 OFF CACHE BOOL "" FORCE)
	set(BUILD_DX12 ON CACHE BOOL "" FORCE)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(BC_USE_OPENMP OFF CACHE BOOL "" FORCE)

	add_subdirectory("${THIRD_PARTY_DIR}/DirectXTex" EXCLUDE_FROM_ALL)
	add_subdirectory("${THIRD_PARTY_DIR}/DirectXMesh" EXCLUDE_FROM_ALL)
	target_link_libraries("OmegaGTE" PRIVATE DirectXTex DirectXMesh)
	target_include_directories("OmegaGTE" PRIVATE
		"${THIRD_PARTY_DIR}/DirectXTex/DirectXTex"
		"${THIRD_PARTY_DIR}/DirectXMesh/DirectXMesh")

	# cgltf is single-header; the implementation TU lives in
	# src/common/MeshParser.cpp behind a CGLTF_IMPLEMENTATION define.
	# OBJ is parsed inline in MeshParser.cpp (no external dep).
	target_include_directories("OmegaGTE" PRIVATE "${THIRD_PARTY_DIR}/cgltf")

	# ATL headers (atlstr.h / ATL::CStringW / ATL::CComPtr) are used throughout
	# GED3D12*.cpp and d3dx12.h, and the headers auto-link atls.lib via
	# `#pragma comment(lib, ...)`. MSBuild on the Visual Studio generator only
	# injects atlmfc/include and atlmfc/lib/<arch> into a vcxproj when
	# <UseOfAtl> is set in <PropertyGroup Label="Configuration">, and CMake
	# doesn't emit that — so the include is unfindable and the linker hits
	# LNK1104 on atls.lib even with the ATL workload installed. Ninja does
	# NOT need this injection: it invokes cl / clang-cl from a vcvars-active
	# shell where INCLUDE and LIB already carry the atlmfc paths. Required
	# component: Visual Studio Installer → "Individual components" →
	# "C++ ATL for latest v143 build tools (x86 & x64)" (or the matching
	# ARM/ARM64 variant).
	if(CMAKE_GENERATOR MATCHES "Visual Studio")
		set(_OMEGA_ATL_BASE "${CMAKE_CXX_COMPILER}")
		get_filename_component(_OMEGA_ATL_BASE "${_OMEGA_ATL_BASE}" DIRECTORY)  # bin/Host<arch>/<arch>
		get_filename_component(_OMEGA_ATL_BASE "${_OMEGA_ATL_BASE}" DIRECTORY)  # bin/Host<arch>
		get_filename_component(_OMEGA_ATL_BASE "${_OMEGA_ATL_BASE}" DIRECTORY)  # bin
		get_filename_component(_OMEGA_ATL_BASE "${_OMEGA_ATL_BASE}" DIRECTORY)  # <tools version>
		set(_OMEGA_ATL_INCLUDE "${_OMEGA_ATL_BASE}/atlmfc/include")
		if(NOT EXISTS "${_OMEGA_ATL_INCLUDE}/atlstr.h" AND DEFINED ENV{VCToolsInstallDir})
			# Fallback for non-standard compiler paths (Build Tools install,
			# external clang-cl, etc.). VS / vcvars / vc-env.bat all set this.
			set(_OMEGA_ATL_INCLUDE "$ENV{VCToolsInstallDir}/atlmfc/include")
		endif()
		if(NOT EXISTS "${_OMEGA_ATL_INCLUDE}/atlstr.h")
			message(FATAL_ERROR
				"OmegaGTE D3D12 backend requires atlstr.h from ATL.\n"
				"  Looked at: ${_OMEGA_ATL_INCLUDE}/atlstr.h\n"
				"Install via the Visual Studio Installer → Individual components →\n"
				"  'C++ ATL for latest v143 build tools (x86 & x64)'\n"
				"(or the matching ARM64 variant for ARM targets). If ATL is\n"
				"already installed, run autom/tools/vc-env.bat before configuring\n"
				"so VCToolsInstallDir points at the right toolset.")
		endif()
		target_include_directories("OmegaGTE" PRIVATE "${_OMEGA_ATL_INCLUDE}")

		# ATL headers auto-link via `#pragma comment(lib, "atls.lib")`, but
		# the matching atlmfc/lib/<arch>/ directory is only added to the
		# linker's search path when <UseOfAtl> is set on the vcxproj. Without
		# this, LNK1104: cannot open file 'atls.lib'. Atlmfc's per-arch lib
		# subdirs are lowercase (x86/x64/arm/arm64) regardless of the msbuild
		# Platform name; map both Win32->x86 and the ARM/ARM64 variants.
		if(CMAKE_GENERATOR_PLATFORM)
			set(_OMEGA_ATL_ARCH "${CMAKE_GENERATOR_PLATFORM}")
		elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
			set(_OMEGA_ATL_ARCH "ARM64")
		elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM|arm)$")
			set(_OMEGA_ATL_ARCH "ARM")
		elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
			set(_OMEGA_ATL_ARCH "x64")
		else()
			set(_OMEGA_ATL_ARCH "Win32")
		endif()
		string(TOLOWER "${_OMEGA_ATL_ARCH}" _OMEGA_ATL_ARCH_LC)
		if(_OMEGA_ATL_ARCH_LC STREQUAL "win32")
			set(_OMEGA_ATL_ARCH_LC "x86")
		endif()
		set(_OMEGA_ATL_LIB_DIR "${_OMEGA_ATL_BASE}/atlmfc/lib/${_OMEGA_ATL_ARCH_LC}")
		if(NOT EXISTS "${_OMEGA_ATL_LIB_DIR}/atls.lib")
			message(FATAL_ERROR
				"OmegaGTE D3D12 backend requires atls.lib from ATL.\n"
				"  Looked at: ${_OMEGA_ATL_LIB_DIR}/atls.lib\n"
				"The ATL workload variant must match the target architecture\n"
				"(${_OMEGA_ATL_ARCH}). Install the matching 'C++ ATL for latest\n"
				"v143 build tools' component via the Visual Studio Installer.")
		endif()
		target_link_directories("OmegaGTE" PRIVATE "${_OMEGA_ATL_LIB_DIR}")

		unset(_OMEGA_ATL_BASE)
		unset(_OMEGA_ATL_INCLUDE)
		unset(_OMEGA_ATL_ARCH)
		unset(_OMEGA_ATL_ARCH_LC)
		unset(_OMEGA_ATL_LIB_DIR)
	endif()
endif()
if(TARGET_METAL)
	target_link_system_frameworks("OmegaGTE" Metal MetalKit ModelIO QuartzCore Cocoa)
endif()

# Tests reference $<TARGET_FILE:omegaslc>, which now resolves to the host
# binary on cross-compile builds via the IMPORTED target above. Test driver
# scripts that try to *run* the test binaries can't do so under cross-compile,
# so we still skip the test subdirs there.
if(NOT CMAKE_CROSSCOMPILING)
	add_subdirectory(omegasl/tests)
	add_subdirectory(tests)
endif()
