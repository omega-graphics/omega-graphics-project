cmake_minimum_required(VERSION 3.13)

include(cmake/OmegaGraphicsSuite.cmake)

project(OmegaGraphics C CXX)

# Allow Objecitve C /Objective C++ to be built for any projects
if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Global compiler diagnostics (C++17). Applies to every subproject added
# below; the language standard itself is set in
# cmake/OmegaGraphicsSuite.cmake. This block must sit after
# project()/enable_language (so CMAKE_CXX_COMPILER_ID is populated) and
# before the omega_graphics_add_subdir() calls (so every target inherits it).
#
# A high-warning baseline is always on. The build-breaking or expensive
# tooling is gated behind opt-in options (default OFF), intended for the CI
# compiler matrix and focused local runs, so a normal dev build stays green.
# ---------------------------------------------------------------------------
option(OMEGA_WARNINGS_AS_ERRORS "Treat warnings as errors (MSVC /WX, GCC/Clang -Werror)." OFF)
option(OMEGA_ENABLE_SANITIZERS  "Build with AddressSanitizer + UBSan (GCC/Clang; compile+link)." OFF)
option(OMEGA_MSVC_ANALYZE       "Enable MSVC /analyze static analysis (MSVC only; slow)." OFF)
option(OMEGA_GCC_ANALYZER       "Enable GCC -fanalyzer static analysis (GCC only; slow)." OFF)
option(OMEGA_CLANG_WEVERYTHING  "Enable Clang -Weverything (triage only; extremely noisy)." OFF)

if(MSVC)
    # /permissive- and /Zc:__cplusplus are the switches that make MSVC
    # actually enforce ISO C++17 (two-phase name lookup, a conforming
    # __cplusplus value) and bring its conformance in line with GCC/Clang.
    # Strip CMake's default /W3 first so /W4 doesn't emit D9025
    # ("overriding '/W3' with '/W4'").
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
    if(OMEGA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
    if(OMEGA_MSVC_ANALYZE)
        add_compile_options(/analyze)
    endif()
else()
    # GCC / Clang / AppleClang. -Wconversion is the load-bearing one for this
    # suite: it catches the LLP64 (Windows) vs LP64 (macOS/Linux) long/pointer
    # narrowing that the .clang-tidy config also targets.
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
    if(OMEGA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    if(OMEGA_ENABLE_SANITIZERS)
        # Must be on both the compile and link lines; ASan/UBSan inject a
        # runtime that the final link has to pull in.
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND OMEGA_GCC_ANALYZER)
        add_compile_options(-fanalyzer)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND OMEGA_CLANG_WEVERYTHING)
        add_compile_options(-Weverything)
    endif()
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
    omega_graphics_add_subdir(OmegaVA va)

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
