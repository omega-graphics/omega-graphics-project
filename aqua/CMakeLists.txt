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

# Cross-platform-identical results: disable floating-point multiply-add
# contraction across the simulation TUs. Clang defaults to `-ffp-contract=on`
# in C++ mode (it fuses `a*b + c` into a single-rounded FMA); MSVC's default
# `/fp:precise` does not contract. That divergence made the same source produce
# different rounding in the solver's dot/cross hot path, and over a settling
# stack's ~1200 chaotic sub-steps it amplified enough to fail aqua_contact_test
# on macOS/Linux (Clang) while it passed on Windows (MSVC) — the resting-impulse
# oracle drifted from its analytic value by 21% (contraction off brings it to
# 0.0%). AQUA's contract is reproducible physics (byte-identical manifolds /
# impulses — see aqua_contact_test's determinism case), which requires every
# backend to round identically, so we forbid the fuse rather than loosen the
# oracle. Contraction is also an active correctness hazard for the geometric
# predicates here: a fused `a*b - c*d` cross product / 2x2 determinant rounds
# its two products differently and can flip sign near zero. Orthogonal to `-O2`
# below — vectorization still applies; only the multiply-add fuse is disabled.
#
# Only real cl.exe needs no flag (its default already forbids the fuse); every
# LLVM/GCC driver — including clang-cl, where CMake's `MSVC` is also true —
# takes `-ffp-contract=off`, so we branch on the compiler *id*, not `MSVC`.
# PRIVATE: only AQUA's own TUs run the simulation; consumers reading results
# back don't need it.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(AQUA PRIVATE -ffp-contract=off)
endif()

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
set(AQUA_KERNEL_STAGES Probe Integrate Broadphase Narrowphase Solver XPBD Scan Particles)
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
option(AQUA_BUILD_DEBUG "Build AQUA in DEBUG mode" OFF)
if(NOT AQUA_BUILD_DEBUG)
    if(MSVC)
        target_compile_options(AQUA PRIVATE /O2 /DNDEBUG>)
    else()
        target_compile_options(AQUA PRIVATE -O2 -DNDEBUG>)
    endif()
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

# Stage the merged AQKernels.omegasllib where its consumers run from: beside
# AQUA's own shared-library artifact (bin/ next to AQUA.dll on Windows, lib/
# next to libAQUA.so/.dylib elsewhere — the same split the AQUA install rules
# use) and beside the aqua test executables (their RUNTIME_OUTPUT_DIRECTORY,
# ${CMAKE_BINARY_DIR}/aqua/tests). OUTPUT-based rather than a POST_BUILD hook
# on AQUA: a kernel-only edit rebuilds AQKernels.omegasllib WITHOUT relinking
# AQUA, and a POST_BUILD hook would leave the staged copies stale in exactly
# that case. Plain `copy` (not copy_if_different) — an OUTPUT rule must
# re-stamp its outputs or ninja re-runs it on every build.
if(WIN32)
    set(_AQUA_KERNELS_STAGE_DIRS "${CMAKE_BINARY_DIR}/bin")
else()
    set(_AQUA_KERNELS_STAGE_DIRS "${CMAKE_BINARY_DIR}/lib")
endif()
if(AQUA_BUILD_TESTS)
    list(APPEND _AQUA_KERNELS_STAGE_DIRS "${CMAKE_BINARY_DIR}/aqua/tests")
endif()
set(_AQUA_KERNELS_STAGED_LIBS)
set(_AQUA_KERNELS_STAGE_CMDS)
foreach(_stage_dir IN LISTS _AQUA_KERNELS_STAGE_DIRS)
    list(APPEND _AQUA_KERNELS_STAGED_LIBS "${_stage_dir}/AQKernels.omegasllib")
    list(APPEND _AQUA_KERNELS_STAGE_CMDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${AQUA_KERNELS_LIB}" "${_stage_dir}/AQKernels.omegasllib")
endforeach()
add_custom_command(OUTPUT ${_AQUA_KERNELS_STAGED_LIBS}
    ${_AQUA_KERNELS_STAGE_CMDS}
    DEPENDS "${AQUA_KERNELS_LIB}"
    COMMENT "Staging AQKernels.omegasllib beside the AQUA runtime library and the aqua tests")
add_custom_target(AQUAKernelLibStage DEPENDS ${_AQUA_KERNELS_STAGED_LIBS})
add_dependencies(AQUAKernelLibStage AQUAKernelLib)
add_dependencies(AQUA AQUAKernelLibStage)

# Ship the kernel lib with the AQUA runtime artifact in the install tree too
# (mirrors install(TARGETS AQUA): RUNTIME->bin on Windows, LIBRARY->lib
# elsewhere).
if(WIN32)
    install(FILES "${AQUA_KERNELS_LIB}" DESTINATION bin)
else()
    install(FILES "${AQUA_KERNELS_LIB}" DESTINATION lib)
endif()
