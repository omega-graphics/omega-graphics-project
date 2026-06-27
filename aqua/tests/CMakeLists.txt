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
