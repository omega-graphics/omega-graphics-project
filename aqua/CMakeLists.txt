omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX)

# AQUA physics library — glob per src component so adding a new file inside
# src/ doesn't require touching this file (matches the suite convention used
# by kreate/ and va/). AQUA is currently standalone: it has no link-time
# dependency on a graphics engine, so its public math types stay its own.
file(GLOB AQUA_SRCS_TOPLEVEL CONFIGURE_DEPENDS src/*.cpp)

add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${AQUA_SRCS_TOPLEVEL}
    DEPENDS OmegaCommon OmegaGTE
)

target_include_directories("AQUA" PUBLIC
	$<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_compile_definitions(AQUA PRIVATE AQUA__BUILD__)

# AQUA-only optimization floor (Phase 3.x build-config fix). The project's
# default `cmake -S . -B build` leaves `CMAKE_BUILD_TYPE` empty, which means
# every TU compiles -O0 with `assert()`s live. That is catastrophic for the
# physics hot path — `aqua_contact_test`'s settling stack runs ~50× slower
# than under -O2 because GTE's matrix/vector operators don't inline, the PGS
# row sweep isn't vectorised, and the `#ifndef NDEBUG` finite-checks in
# `AQStepBodyPosition` fire on every body every sub-step. The cost was
# misdiagnosed as a hang during the Phase 3.x Nesterov-GJK landing;
# diagnosing it surfaced the broader issue that the project's tests stress
# code paths the default config can't honestly profile.
#
# We force `-O2 -DNDEBUG` on AQUA only — narrowly, so this fix doesn't
# perturb the build of OmegaCommon / OmegaGTE / WTK / kREATE. The guard
# `$<NOT:$<CONFIG:Debug>>` means an explicit `-DCMAKE_BUILD_TYPE=Debug`
# still gets the debugger-friendly unoptimized build. Empty config (the
# project default) is *not* Debug, so it picks up the optimizer here — the
# only change visible to a stock checkout. Release / RelWithDebInfo append
# this on top of the preset's own `-O3`/`-O2`; clang takes the last `-O`
# wins, which means Release sees `-O2` instead of `-O3`. The ~5% delta vs.
# `-O3` is dwarfed by the ~50× recovery vs. `-O0`; if a future profile
# needs `-O3` specifically, tighten the condition then.
#
# CMake's `MSVC` variable is set for both cl.exe and clang-cl.exe (clang's
# MSVC-compatible driver), and both reject `-O2`/`-DNDEBUG` — they only
# understand the slash-style switches. Branch on the driver so the same
# AQUA-only floor lands on Windows-toolchain builds as well: `/O2` is the
# standard "maximize speed" preset (auto-inline, intrinsics, loop opt) and
# `/DNDEBUG` strips `assert()` the same way `-DNDEBUG` does in the
# GCC/Clang branch. clang-cl maps `/O2` onto the same LLVM passes as
# `-O2`, so the physics hot-path recovery is the same as on Linux.
if(MSVC)
    target_compile_options(AQUA PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2 /DNDEBUG>)
else()
    target_compile_options(AQUA PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2 -DNDEBUG>)
endif()

# NOTE: No third-party physics SDK is currently vendored. Whether AQUA grows its
# own solver or integrates an external SDK is an open decision — if it integrates
# one, add the dependency here and the clone entry to AUTOMDEPS, and keep the
# backend types out of include/aqua/.

# Phase 1 deliverable + math/parity unit tests. Opt-out with -DAQUA_BUILD_TESTS=OFF.
option(AQUA_BUILD_TESTS "Build AQUA unit/validation tests" ON)
if(AQUA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
