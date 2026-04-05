# cmake_minimum_required(VERSION 3.9)

set(OMEGAGTE_VERSION "0.5")

omega_graphics_project(OmegaGTE VERSION ${OMEGAGTE_VERSION} LANGUAGES CXX)


include(./OmegaGTE.cmake)

file(GLOB MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB COMMON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp")

set(PUBLIC_DEFS)

if(WIN32)

	set(TARGET_DIRECTX TRUE)

	set(PUBLIC_DEFS "TARGET_DIRECTX")

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

	if(ANDROID)
		add_compile_definitions("VULKAN_TARGET_ANDROID")
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
		add_compile_definitions("VULKAN_TARGET_WAYLAND")
	elseif(NOT ANDROID)
		message("OmegaGTE: Linux Vulkan backend = X11")
		add_compile_definitions("VULKAN_TARGET_X11")
	endif()


	set(PUBLIC_DEFS "TARGET_VULKAN")

	file(GLOB RHI_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/*.cpp")
endif()

set(MAIN_SRCS ${MAIN_SRCS} ${COMMON_SRCS} ${RHI_SRCS})

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
	set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
	set(VULKAN_SDK_DIR ${THIRD_PARTY_DIR}/vulkan_sdk/1.3.283.0)

	set(VULKAN_INCLUDE_DIRECTORY "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/include")
	set(VMA_INCLUDE_DIRECTORY "${THIRD_PARTY_DIR}/vulkan-memory-allocator/include")
	set(GLM_INCLUDE_DIRECTORY "${THIRD_PARTY_DIR}/glm")

	set(VULKAN_SDK_GLSLC "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/bin/glslc")

	# Copy glslc into the build bin/ directory so omegaslc (and tests) can find it.
	file(COPY "${VULKAN_SDK_GLSLC}" DESTINATION "${CMAKE_BINARY_DIR}/bin"
		 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

	target_compile_definitions("omegaslc" PRIVATE "OMEGASL_DEFAULT_GLSLC=\"${CMAKE_BINARY_DIR}/bin/glslc\"")

	# Bundle glslc into the installed distribution.
	install(PROGRAMS "${VULKAN_SDK_GLSLC}" DESTINATION bin)

	target_link_directories("OmegaGTE" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib")
	target_link_libraries("omegaslc" PRIVATE "${VULKAN_SDK_DIR}/${CMAKE_SYSTEM_PROCESSOR}/lib/libshaderc_shared.so.1")
	target_include_directories("omegaslc" PRIVATE ${VULKAN_INCLUDE_DIRECTORY})
	target_include_directories("OmegaGTE" PRIVATE  ${VULKAN_INCLUDE_DIRECTORY} ${VMA_INCLUDE_DIRECTORY} ${GLM_INCLUDE_DIRECTORY})
	target_link_libraries("OmegaGTE" PRIVATE libvulkan.so.1 libshaderc_shared.so)
endif()
if(TARGET_METAL)
	target_link_system_frameworks("omegaslc" Metal Foundation)
	target_link_system_frameworks("OmegaGTE" Metal QuartzCore Cocoa)
endif()

 add_subdirectory(omegasl/tests)
 add_subdirectory(tests)
