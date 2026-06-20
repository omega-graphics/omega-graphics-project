# OmegaCommon core-runtime unit tests.
# Home for assertion-style tests of the shared runtime types (JSON, and future
# core surfaces). Wired into CTest; run with `ctest -R json_lifecycle` or by
# invoking the built binary in ${CMAKE_BINARY_DIR}/tests directly.
enable_testing()

add_executable(json-lifecycle-test JSONLifecycleTest.cpp)
set_target_properties(json-lifecycle-test PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(json-lifecycle-test OmegaCommonCore)
target_link_libraries(json-lifecycle-test PRIVATE OmegaCommonCore)

add_test(NAME json_lifecycle COMMAND json-lifecycle-test)

add_custom_target(common-core-tests
	COMMAND json-lifecycle-test
	DEPENDS json-lifecycle-test
	COMMENT "Running OmegaCommon core-runtime unit tests")
