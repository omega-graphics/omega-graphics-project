cmake_minimum_required(VERSION 3.13)

include(cmake/OmegaGraphicsSuite.cmake)

project(OmegaGraphics C CXX)

# Allow Objecitve C /Objective C++ to be built for any projects
if(APPLE)
    enable_language(OBJCXX)
endif()

# OMEGA_HOST_TOOLS_ONLY — internal flag for the host-tools superbuild.
#
# Set to ON automatically by `omega_init_host_tools` in
# cmake/OmegaGraphicsSuite.cmake when a cross-compile parent build invokes
# the superbuild via ExternalProject_Add. Not intended for direct user
# invocation; pass it on the cmake command line only when reproducing the
# host-tools build by hand.
#
# When ON:
#   - Configures with the host's default compilers (no toolchain file).
#   - Builds AUTOM, OmegaCommon, and the omega graphics tools registered via
#     add_omega_graphics_tool (autom, autom-install, omegaslc, omega-wrapgen,
#     omega-assetc, omega-ebin, parse-test). These produce host binaries that
#     the cross-compile parent copies into its own bin/ via a POST_BUILD step.
#   - Skips OmegaWTK and AQUA — they are runtime libraries with no host-tool
#     contribution.
#   - Skips OmegaGTE's library/framework build (only its omegaslc tool path
#     is needed).
#
# When OFF (default): full normal build for the configured target.
option(OMEGA_HOST_TOOLS_ONLY "Internal: build only the host-side dev tools (autom, omegaslc, etc.). Set automatically by the cross-compile host superbuild." OFF)

# AUTOM is host-side build tooling. On cross-compile to iOS/Android nothing
# on-device consumes it; the host-tools superbuild (driven by
# add_omega_graphics_tool) handles the autom CLI. Always build it for the
# host-tools superbuild itself though, so the host binaries actually land.
if(OMEGA_HOST_TOOLS_ONLY OR NOT CMAKE_CROSSCOMPILING)
    omega_graphics_add_subdir(AUTOM autom)
endif()

omega_graphics_add_subdir(OmegaCommon common)

omega_graphics_add_subdir(OmegaGTE gte)

if(NOT OMEGA_HOST_TOOLS_ONLY)
    omega_graphics_add_subdir(OmegaWTK wtk)

    # Add Game engine module. AQUA cross-compiles for iOS and Android via
    # the platform stubs in src/platform/{ios,android}; other cross-compile
    # targets are still excluded until they grow platform code.
    if(NOT CROSS_COMPILE
       OR CMAKE_SYSTEM_NAME STREQUAL "iOS"
       OR CMAKE_SYSTEM_NAME STREQUAL "Android")
        omega_graphics_add_subdir(AQUA aqua)
    endif()
endif()
