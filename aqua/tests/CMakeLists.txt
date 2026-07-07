enable_testing()

include(CMakeParseArguments)

# add_aqua_test(NAME <exe> SOURCES <src...> [LIBS <lib...>])
#
# Wraps the per-test boilerplate so a new AQUA validation binary is one call.
# Every test is built into ${CMAKE_BINARY_DIR}/aqua/tests/ — a dedicated test
# tree rather than the project's shared bin/. On Windows the loader looks for
# dependent DLLs next to the .exe (no rpath equivalent), so we stage every
# project DLL there via omega_stage_runtime_dlls(): AQUA itself, OmegaCommon
# (plus its ICU fan-out), OmegaGTE, and anything else the suite stages into
# ${CMAKE_BINARY_DIR}/bin/. Without this step the AQUA tests fail at launch
# with DLL_NOT_FOUND because the per-module RUNTIME_OUTPUT_DIRECTORY scatters
# the .dlls across bin/ while CTest invokes the .exe from aqua/tests/. No-op
# elsewhere (rpath / @loader_path colocate the libs on Linux and macOS).
#
# The CTest name is the executable name with the trailing `_test` stripped,
# preserving the convention from the hand-written list this macro replaces
# (aqua_math_test -> aqua_math, aqua_dynamics_test -> aqua_dynamics, ...).
function(add_aqua_test)
    cmake_parse_arguments(_ARG "" "NAME" "SOURCES;LIBS" ${ARGN})

    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    target_include_directories(${_ARG_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")

    if(_ARG_LIBS)
        target_link_libraries(${_ARG_NAME} PRIVATE ${_ARG_LIBS})
    endif()

    # Explicit AQUA build ordering when the test consumes it. target_link_libraries
    # already adds the dep edge, but the original hand-written list set this
    # explicitly and keeping it makes the parallel build deterministic if the
    # test ever stops linking AQUA directly.
    if("AQUA" IN_LIST _ARG_LIBS)
        add_dependencies(${_ARG_NAME} AQUA)
    endif()

    set_target_properties(${_ARG_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aqua/tests")

    omega_stage_runtime_dlls(${_ARG_NAME})

    string(REGEX REPLACE "_test$" "" _ctest_name "${_ARG_NAME}")
    add_test(NAME ${_ctest_name} COMMAND ${_ARG_NAME})
endfunction()


# Phase 1 math core + integrator validation. Header-only (AQMath.h /
# AQIntegrator.h are templates with no link-time dependency), pure CPU — runs on
# any host regardless of GPU backend. This is the §6 CPU/GPU parity seed.
add_aqua_test(
    NAME    aqua_math_test
    SOURCES aqua_math_test.cpp
    LIBS    OmegaCommonCore)

# Integration test through the public AQUA surface. Links the AQUA library
# (which has no link-time GTE dependency — the math is header-only), driving the
# Phase 1 deliverable scene through AQContext/AQSpace/AQRigidBody.
add_aqua_test(
    NAME    aqua_dynamics_test
    SOURCES aqua_dynamics_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 1.1 rigid-body completion validation: AABB math + oriented-box bound,
# full-tensor parity, conserved-quantity accessors, adaptive-iteration drift
# reduction, damping/clamp behaviour + Phase 1 regression guard, debug stream
# round-trip. Goes through the public AQContext/AQSpace/AQRigidBody surface to
# cover the runnable-deliverable bar in Phase-1.1 §1.
add_aqua_test(
    NAME    aqua_rigid_body_test
    SOURCES aqua_rigid_body_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 2 collision-shapes + broadphase validation: shape factories +
# inertia-from-shape closure, brute-force oracle parity (static + moving),
# rotation-correct AABB on a spinning box, determinism, layer/mask filter,
# COM-offset torque-arm wiring, and a scaling log. Drives the public AQUA
# surface end-to-end, matching the Phase-2 brief §1 runnable-deliverable bar.
add_aqua_test(
    NAME    aqua_broadphase_test
    SOURCES aqua_broadphase_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 3 narrowphase + contact-solver validation: settling stack, incline
# friction (stay + slide), sphere bounce restitution, GJK/EPA hull pair,
# determinism, and energy non-growth on a settled stack. Drives the same
# public AQContext/AQSpace/AQRigidBody surface as the earlier phase tests
# and asserts the Phase-3 brief §1/§9 runnable-deliverable bars verbatim.
add_aqua_test(
    NAME    aqua_contact_test
    SOURCES aqua_contact_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 4 joints / queries / sleeping / CCD validation: the swinging bridge
# (ball-socket chain + catenary support-force oracle), the hinge door (angular
# limit + motor steady state), raycast/shapecast/overlap + island sleep/wake,
# and the CCD bullet (off tunnels / speculative < 1 cm / continuous < 1 mm).
# Drives the public AQContext/AQSpace/AQRigidBody surface, matching the Phase-4
# brief §1/§9 runnable-deliverable bars.
add_aqua_test(
    NAME    aqua_phase4_test
    SOURCES aqua_phase4_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 6 §13.3 6a: closed-form point-vs-shape signed-distance validation. The
# AQshapeSignedDistance query the particle push-out rides on — pinned against
# hand-computed distances/normals for plane/sphere/box/capsule with translated
# and rotated bodies. Pure CPU (the math is header-only; the query is in libAQUA).
add_aqua_test(
    NAME    aqua_particle_sdf_test
    SOURCES aqua_particle_sdf_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 6 §13.3 6b: particle pool / free-list / stable-compaction census. INTERNAL
# test — drives the private AQParticleSystem (src/AQSpaceImpl.h) directly, so it
# adds ../src to the include path. Pure CPU (no kernel lib, no GPU): asserts the
# §9 census invariants — conservation, stable survivor order, recycle-exactly-once,
# deterministic reuse, and the NaN guard.
add_aqua_test(
    NAME    aqua_particle_pool_test
    SOURCES aqua_particle_pool_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_particle_pool_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 6 §13.3 6c: emission + force fields + integration. INTERNAL test — drives
# AQParticleSystem + the scalar-generic particle math (src/AQParticleMath.h)
# directly. Pure CPU: per-field acceleration, semi-implicit closed form,
# deterministic emission (count carry + bitwise attribute reproducibility),
# spawn bounds, aging, and a whole-pipeline census smoke.
add_aqua_test(
    NAME    aqua_particle_step_test
    SOURCES aqua_particle_step_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_particle_step_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 6 §13.3 6d: public particle API + one-way collision + step wiring. PUBLIC
# test — drives AQContext/AQSpace exactly as kREATE would (no src/ internals):
# API round-trip, the collision no-tunneling hard invariant, the collision toggle,
# and determinism through the public surface.
add_aqua_test(
    NAME    aqua_particle_api_test
    SOURCES aqua_particle_api_test.cpp
    LIBS    AQUA OmegaCommonCore)

# Phase 6 §1/§9 6e: the runnable deliverable — a particle fountain validated
# against a double-precision reference oracle that replays the same pipeline in
# sequential double code (reusing AQParticleMath.h + AQParticleCollision.h).
# INTERNAL (the oracle needs the shared generic math): trajectory-vs-oracle band,
# no-tunneling against floor/box/sphere (exact), census match, scale, debug bus.
add_aqua_test(
    NAME    aqua_particle_test
    SOURCES aqua_particle_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_particle_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 7 §13 7b: XPBD storage + authoring + deterministic graph coloring.
# INTERNAL test — drives the private AQXPBDBody (src/AQSpaceImpl.h) directly
# plus the public authoring surface. Pure CPU: conflict-free color batches,
# chain/star closed forms, coloring determinism, topology-change recolor,
# public round-trip.
add_aqua_test(
    NAME    aqua_xpbd_color_test
    SOURCES aqua_xpbd_color_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_xpbd_color_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 7 §13 7c: the XPBD projection math + slice loop. INTERNAL test — the
# scalar-generic src/AQXPBDMath.h against hand-computed closed forms (rigid /
# compliant / pinned / degenerate / guard-clamped projections), then
# AQXPBDBody::advance semantics (free-fall closed form, pendulum rigidity,
# loud guard trips, bitwise determinism).
add_aqua_test(
    NAME    aqua_xpbd_step_test
    SOURCES aqua_xpbd_step_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_xpbd_step_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 7 §1/§9: the runnable deliverable — a rope under XPBD validated against
# double-precision physical oracles (measurement off the hot path, §8): energy
# no-injection + monotone settle, catenary fit, alpha = 0 inextensibility,
# timestep-independent stretch (60 vs 240 Hz — the headline XPBD property),
# analytic per-segment stretch alpha*T_i, determinism, and the loud guard.
# INTERNAL (the oracle shares src/AQXPBDMath.h); authoring goes through the
# public AQContext/AQSpace surface exactly as kREATE would.
add_aqua_test(
    NAME    aqua_xpbd_rope_test
    SOURCES aqua_xpbd_rope_test.cpp
    LIBS    AQUA OmegaCommonCore)
target_include_directories(aqua_xpbd_rope_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Phase 5a compute-substrate groundwork: execution-path selection + the
# CreateCPUOnly / mandatory-engine Create(engine, queue) factory split. The
# CPU-only assertions always run; the guarded mandatory-engine section inits a
# real GTE device when one exists (skips on headless CI). Links OmegaGTE for the
# Init / Close / enumerateDevices entry points and inherits the TARGET_* platform
# macro from AQUA's PUBLIC compile definition (so <omegaGTE/GTEDevice.h> compiles).
add_aqua_test(
    NAME    aqua_exec_path_test
    SOURCES aqua_exec_path_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)

# aqua_exec_path_test is the first AQUA test to link OmegaGTE, which on macOS is a
# framework staged in ${CMAKE_BINARY_DIR}/Frameworks — NOT build/lib, where the
# other dylibs (libAQUA, libOmegaCommonCore) live and where add_aqua_test's rpath
# points. Add the Frameworks dir to this test's runtime search path so it actually
# loads at launch. No-op on Linux/Windows where OmegaGTE is a plain .so/.dll under
# build/lib. (The GTE suite's own framework-linking tests have the same gap.)
if(APPLE)
    set_property(TARGET aqua_exec_path_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5b: SoA body mirror + the precompiled-kernel toolchain. INTERNAL test —
# includes AQUA's src/ headers (AQBodySoA.h, AQComputeBackend.h) directly, so it
# adds ../src to the include path. The CPU SoA round-trip always runs; the
# device-guarded section loads the precompiled AQKernels.omegasllib (its build
# path injected as AQUA_KERNELS_LIB_PATH) and runs AQComputeBackend's capability
# probe. Depends on AQUAKernelLib so the lib is built first.
add_aqua_test(
    NAME    aqua_compute_test
    SOURCES aqua_compute_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)
target_include_directories(aqua_compute_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(aqua_compute_test PRIVATE "AQUA_KERNELS_LIB_PATH=\"${AQUA_KERNELS_LIB}\"")
add_dependencies(aqua_compute_test AQUAKernelLib)
if(APPLE)
    set_property(TARGET aqua_compute_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5c: integration-kernel stage-isolation parity (plan §9/§13 5c) — the
# GPU AQIntegrateVelocity/AQIntegratePosition kernels vs the CPU AQIntegrator
# templates from identical state, plus the within-path byte-identical re-run.
# Same INTERNAL-test shape as aqua_compute_test (src/ headers + kernel-lib
# path via compile definition); device-guarded, skips on headless CI.
add_aqua_test(
    NAME    aqua_gpu_integrate_test
    SOURCES aqua_gpu_integrate_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)
target_include_directories(aqua_gpu_integrate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(aqua_gpu_integrate_test PRIVATE "AQUA_KERNELS_LIB_PATH=\"${AQUA_KERNELS_LIB}\"")
add_dependencies(aqua_gpu_integrate_test AQUAKernelLib)
if(APPLE)
    set_property(TARGET aqua_gpu_integrate_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5e: narrowphase stage-isolation parity (plan §9/§13 5e) — the GPU
# count -> prefix-scan -> build chain vs the CPU narrowphase's manifolds and a
# host recomputation of the CPU row-build; plus the warm-start cache round
# trip and the cpuFallback flags for GJK pairs. Device-guarded.
add_aqua_test(
    NAME    aqua_gpu_narrowphase_test
    SOURCES aqua_gpu_narrowphase_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)
target_include_directories(aqua_gpu_narrowphase_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(aqua_gpu_narrowphase_test PRIVATE "AQUA_KERNELS_LIB_PATH=\"${AQUA_KERNELS_LIB}\"")
add_dependencies(aqua_gpu_narrowphase_test AQUAKernelLib)
if(APPLE)
    set_property(TARGET aqua_gpu_narrowphase_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5f: colored-solver stage parity (plan §9/§13 5f) — one full GPU
# sub-step (velocity half-step -> narrowphase -> CPU-colored GPU PGS +
# split-impulse -> position half-step) on a penetrating box stack vs one CPU
# advance; state within the cross-path band, impulses within the
# measured-relaxed band, full-sub-step re-run byte-identical. Device-guarded.
add_aqua_test(
    NAME    aqua_gpu_solver_test
    SOURCES aqua_gpu_solver_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)
target_include_directories(aqua_gpu_solver_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(aqua_gpu_solver_test PRIVATE "AQUA_KERNELS_LIB_PATH=\"${AQUA_KERNELS_LIB}\"")
add_dependencies(aqua_gpu_solver_test AQUAKernelLib)
if(APPLE)
    set_property(TARGET aqua_gpu_solver_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()

# Phase 5d: broadphase stage-isolation parity (plan §9/§13 5d) — the GPU grid
# chain (AABB refresh -> hash -> stable sort -> pair scan -> pair sort) vs the
# CPU broadphase's candidate list on a randomized static scene; the ordered
# pair lists must be identical. Device-guarded like the other GPU tests.
add_aqua_test(
    NAME    aqua_gpu_broadphase_test
    SOURCES aqua_gpu_broadphase_test.cpp
    LIBS    AQUA OmegaGTE OmegaCommonCore)
target_include_directories(aqua_gpu_broadphase_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(aqua_gpu_broadphase_test PRIVATE "AQUA_KERNELS_LIB_PATH=\"${AQUA_KERNELS_LIB}\"")
add_dependencies(aqua_gpu_broadphase_test AQUAKernelLib)
if(APPLE)
    set_property(TARGET aqua_gpu_broadphase_test APPEND PROPERTY
        BUILD_RPATH "${CMAKE_BINARY_DIR}/Frameworks")
endif()
