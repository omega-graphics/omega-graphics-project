omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX)

# AQUA physics library — glob per src component so adding a new file inside
# src/ doesn't require touching this file (matches the suite convention used
# by kreate/ and va/). Through Phase 4 AQUA used only OmegaGTE's header-only
# math; Phase 5 (compute substrate) adds the GPU path, so AQUA now compiles
# against the GTE GPU headers (hence the target-platform macro below) and will
# link OmegaGTE for the kernel dispatch in 5b/5c. The public math types stay
# AQUA's own; no OmegaGTE backend type leaks into include/aqua/.
file(GLOB AQUA_SRCS_TOPLEVEL CONFIGURE_DEPENDS src/*.cpp)

add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        ${AQUA_SRCS_TOPLEVEL}
    DEPENDS OmegaCommonCore OmegaGTE
)

target_include_directories("AQUA" PUBLIC
	$<TARGET_PROPERTY:OmegaCommonCore,INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)
target_compile_definitions(AQUA PRIVATE AQUA__BUILD__)

# Phase 5 (compute substrate): AQUA dispatches OmegaSL compute kernels through
# OmegaGTE, so its TUs now include <omegaGTE/GE.h>, which hard-#errors unless the
# target-platform macro is defined (TARGET_METAL / TARGET_DIRECTX / TARGET_VULKAN —
# see gte/include/omegaGTE/GE.h:7). The suite helper wires OmegaGTE as a build-order
# DEPENDS (add_dependencies), which does NOT propagate OmegaGTE's PUBLIC TARGET_*
# definition, so AQUA selects it itself, mirroring gte/CMakeLists.txt's per-platform
# branch. PUBLIC so AQUA's own tests/consumers that include GTE GPU headers (e.g.
# <omegaGTE/GTEDevice.h>) inherit it too.
if(WIN32)
    # NOMINMAX matches GTE: GE.h pulls in <windows.h> on D3D12, whose min/max
    # macros would otherwise clobber std::min/std::max.
    target_compile_definitions(AQUA PUBLIC TARGET_DIRECTX NOMINMAX)
elseif(APPLE)
    target_compile_definitions(AQUA PUBLIC TARGET_METAL)
else()
    # Linux/Android → Vulkan. AQUA uses compute only (no surfaces), so the GE.h
    # VULKAN_TARGET_{WAYLAND,X11,ANDROID} window-system sub-macros are not needed
    # to compile the compute path; add one here if a future AQUA TU includes the
    # Vulkan surface headers.
    target_compile_definitions(AQUA PUBLIC TARGET_VULKAN)
endif()

# Phase 5b: AQUA now LINKS OmegaGTE (was build-order DEPENDS only — the suite
# helper's add_dependencies links nothing). The compute backend calls real GTE
# symbols (makeBuffer, makeComputePipelineState, loadShaderLibrary,
# GEBufferWriter/Reader). PRIVATE: OmegaGTE is an implementation detail — no
# OmegaGTE backend type appears in include/aqua/* (the pimpl discipline holds),
# so consumers of AQUA do not need it on their link line.
target_link_libraries(AQUA PRIVATE OmegaGTE)

# On macOS OmegaGTE is a *framework* staged in ${CMAKE_BINARY_DIR}/Frameworks,
# not build/lib. Give libAQUA an rpath there so its OmegaGTE.framework dependency
# resolves wherever libAQUA is loaded from — covering every test/consumer that
# loads libAQUA transitively (dyld resolves a dylib's @rpath deps using that
# dylib's own LC_RPATHs). No-op where OmegaGTE is a plain .so/.dll under build/lib.
if(APPLE)
    set_property(TARGET AQUA APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5b/5c: precompile AQUA's compute kernels, loaded at runtime as a single
# `.omegasllib` via loadShaderLibrary (the decision in plan §7.5/§11.4). The
# kernels are SPLIT per pipeline stage (developer direction, 2026-07-01): one
# `.omegasl` source per stage sharing math helpers + common structs through
# `AQKernelsCommon.omegaslh`, each compiled to its own lib, then merged into
# the ONE AQKernels.omegasllib by `omegaslc --link` (a pure container merge).
# AQUA_KERNELS_LIB is exported (CACHE INTERNAL) so the tests can load it by path.
set(AQUA_KERNELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels")
set(AQUA_KERNELS_COMMON "${AQUA_KERNELS_DIR}/AQKernelsCommon.omegaslh")
set(AQUA_KERNELS_LIB "${CMAKE_CURRENT_BINARY_DIR}/AQKernels.omegasllib" CACHE INTERNAL "")

# One entry per stage source; each becomes AQUAKernel<Stage>Lib and a
# per-stage .omegasllib under the build dir.
set(AQUA_KERNEL_STAGES Probe Integrate Broadphase Narrowphase Solver)
set(AQUA_KERNEL_STAGE_LIBS)
foreach(_stage IN LISTS AQUA_KERNEL_STAGES)
    set(_stage_src "${AQUA_KERNELS_DIR}/AQ${_stage}.omegasl")
    set(_stage_lib "${CMAKE_CURRENT_BINARY_DIR}/AQ${_stage}.omegasllib")
    add_omegasl_lib(AQUAKernel${_stage}Lib "${_stage_src}" "${_stage_lib}"
        EXTRA_DEPENDS "${AQUA_KERNELS_COMMON}")
    add_dependencies(AQUAKernel${_stage}Lib omegaslc)
    list(APPEND AQUA_KERNEL_STAGE_LIBS "${_stage_lib}")
endforeach()

add_omegasl_linked_lib(AQUAKernelLib "${AQUA_KERNELS_LIB}"
    SOURCES ${AQUA_KERNEL_STAGE_LIBS})
add_dependencies(AQUAKernelLib omegaslc)
add_dependencies(AQUA AQUAKernelLib)

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
