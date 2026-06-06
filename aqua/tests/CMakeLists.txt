enable_testing()

# Phase 1 math core + integrator validation. Header-only (AQMath.h /
# AQIntegrator.h are templates with no link-time dependency), pure CPU — runs on
# any host regardless of GPU backend. This is the §6 CPU/GPU parity seed.
add_executable(aqua_math_test aqua_math_test.cpp)
target_include_directories(aqua_math_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(aqua_math_test PRIVATE OmegaCommon)
set_target_properties(aqua_math_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME aqua_math COMMAND aqua_math_test)

# Integration test through the public AQUA surface. Links the AQUA library
# (which has no link-time GTE dependency — the math is header-only), driving the
# Phase 1 deliverable scene through AQContext/AQSpace/AQRigidBody.
add_executable(aqua_dynamics_test aqua_dynamics_test.cpp)
target_include_directories(aqua_dynamics_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(aqua_dynamics_test PRIVATE AQUA OmegaCommon)
add_dependencies(aqua_dynamics_test AQUA)
set_target_properties(aqua_dynamics_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME aqua_dynamics COMMAND aqua_dynamics_test)

# Phase 1.1 rigid-body completion validation: AABB math + oriented-box bound,
# full-tensor parity, conserved-quantity accessors, adaptive-iteration drift
# reduction, damping/clamp behaviour + Phase 1 regression guard, debug stream
# round-trip. Goes through the public AQContext/AQSpace/AQRigidBody surface to
# cover the runnable-deliverable bar in Phase-1.1 §1.
add_executable(aqua_rigid_body_test aqua_rigid_body_test.cpp)
target_include_directories(aqua_rigid_body_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(aqua_rigid_body_test PRIVATE AQUA OmegaCommon)
add_dependencies(aqua_rigid_body_test AQUA)
set_target_properties(aqua_rigid_body_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME aqua_rigid_body COMMAND aqua_rigid_body_test)

# Phase 2 collision-shapes + broadphase validation: shape factories +
# inertia-from-shape closure, brute-force oracle parity (static + moving),
# rotation-correct AABB on a spinning box, determinism, layer/mask filter,
# COM-offset torque-arm wiring, and a scaling log. Drives the public AQUA
# surface end-to-end, matching the Phase-2 brief §1 runnable-deliverable bar.
add_executable(aqua_broadphase_test aqua_broadphase_test.cpp)
target_include_directories(aqua_broadphase_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(aqua_broadphase_test PRIVATE AQUA OmegaCommon)
add_dependencies(aqua_broadphase_test AQUA)
set_target_properties(aqua_broadphase_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME aqua_broadphase COMMAND aqua_broadphase_test)

# Phase 3 narrowphase + contact-solver validation: settling stack, incline
# friction (stay + slide), sphere bounce restitution, GJK/EPA hull pair,
# determinism, and energy non-growth on a settled stack. Drives the same
# public AQContext/AQSpace/AQRigidBody surface as the earlier phase tests
# and asserts the Phase-3 brief §1/§9 runnable-deliverable bars verbatim.
add_executable(aqua_contact_test aqua_contact_test.cpp)
target_include_directories(aqua_contact_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gte/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(aqua_contact_test PRIVATE AQUA OmegaCommon)
add_dependencies(aqua_contact_test AQUA)
set_target_properties(aqua_contact_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME aqua_contact COMMAND aqua_contact_test)
