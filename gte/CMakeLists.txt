cmake_minimum_required(VERSION 3.9)

include(OmegaGraphicsSuite)

set(OMEGAGTE_VERSION "0.5")

project(OmegaGTE VERSION ${OMEGAGTE_VERSION} LANGUAGES CXX)

omega_graphics_add_subdir(OmegaCommon common)


include(./OmegaGTE.cmake)

file(GLOB MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

set(PUBLIC_DEFS)

if(WIN32)

	set(TARGET_DIRECTX TRUE)

	set(PUBLIC_DEFS "TARGET_DIRECTX")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/*.cpp")

elseif(APPLE)

	set(TARGET_METAL TRUE)

	set(PUBLIC_DEFS "TARGET_METAL")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/metal/*.mm")

else()
	set(TARGET_VULKAN TRUE)

	find_package(PkgConfig)

	pkg_check_modules(PKG_WAYLAND wayland-client wayland-server wayland-egl wayland-cursor)


	if(PKG_WAYLAND_FOUND)
		add_compile_definitions("VULKAN_TARGET_WAYLAND")
	elseif(ANDROID)
		add_compile_definitions("VULKAN_TARGET_ANDROID")
	endif()

	if(NOT ANDROID)
		add_compile_definitions("VULKAN_TARGET_X11")
	endif()


	set(PUBLIC_DEFS "TARGET_VULKAN")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/*.cpp")
endif()

set(MAIN_SRCS ${MAIN_SRCS} ${RHI_SRCS})

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

file(GLOB OMEGASL_SRCS "omegasl/src/*.h" "omegasl/src/*.cpp")
if(TARGET_METAL)
	set(OMEGASL_SRCS ${OMEGASL_SRCS} "omegasl/src/MetalShaderCompile.mm")
endif()

add_omega_graphics_tool("omegaslc" SOURCES ${OMEGASL_SRCS} LIBS "OmegaCommon")
target_include_directories("omegaslc" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(OMEGASLC_RUNTIME_COMPILE TRUE)



add_omega_graphics_module("OmegaGTE" FRAMEWORK INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" VERSION "0.5" HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" DEPENDS "OmegaCommon" SOURCES ${MAIN_SRCS})
target_compile_definitions("OmegaGTE" PRIVATE OMEGAGTE__BUILD__)
target_compile_definitions("OmegaGTE" PUBLIC ${PUBLIC_DEFS})
target_compile_definitions("omegaslc" PUBLIC ${PUBLIC_DEFS})
if (${OMEGASLC_RUNTIME_COMPILE})
	target_compile_definitions("OmegaGTE" PUBLIC "RUNTIME_SHADER_COMP_SUPPORT=1")
	target_compile_definitions("omegaslc" PUBLIC "RUNTIME_SHADER_COMP_SUPPORT=1")
	target_sources("OmegaGTE" PRIVATE ${OMEGASL_SRCS})
endif ()
target_link_libraries("OmegaGTE" PUBLIC "OmegaCommon")

if(TARGET_VULKAN)
	set(VULKAN_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external-libs/vulkan_sdk/1.2.189.0)
	set(VULKAN_INCLUDE_DIRECTORY "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/include")
	target_link_directories("OmegaGTE" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib")
	target_link_libraries("omegaslc" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib/libshaderc_shared.so.1")
	target_include_directories("omegaslc" PRIVATE ${VULKAN_INCLUDE_DIRECTORY})
	target_include_directories("OmegaGTE" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external-libs/vulkan-memory-allocator/include" "${CMAKE_CURRENT_SOURCE_DIR}/external-libs/glm" ${VULKAN_INCLUDE_DIRECTORY})
	target_link_libraries("OmegaGTE" PRIVATE libvulkan.so.1 libshaderc_shared.so)
endif()
if(TARGET_METAL)
	target_link_system_frameworks("omegaslc" Metal Foundation)
	target_link_system_frameworks("OmegaGTE" Metal QuartzCore Cocoa)
endif()

 add_subdirectory(tests)

