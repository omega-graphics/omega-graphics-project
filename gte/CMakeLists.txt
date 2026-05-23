# cmake_minimum_required(VERSION 3.9)

set(OMEGAGTE_VERSION "0.6")

omega_graphics_project(OmegaGTE VERSION ${OMEGAGTE_VERSION} LANGUAGES CXX)


include(./OmegaGTE.cmake)

file(GLOB MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB COMMON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp")

# MeshParser only handles formats Metal already covers via Model I/O / MTKMesh
# (OBJ, glTF). Drop it from the Metal build until it grows a parser for a
# format Metal can't load (e.g. FBX).
if(APPLE)
	list(REMOVE_ITEM COMMON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/MeshParser.cpp")
endif()

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
