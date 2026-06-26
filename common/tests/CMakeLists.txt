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

add_executable(json-number-test JSONNumberTest.cpp)
set_target_properties(json-number-test PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(json-number-test OmegaCommonCore)
target_link_libraries(json-number-test PRIVATE OmegaCommonCore)

add_test(NAME json_number COMMAND json-number-test)

add_executable(json-lookup-test JSONLookupTest.cpp)
set_target_properties(json-lookup-test PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(json-lookup-test OmegaCommonCore)
target_link_libraries(json-lookup-test PRIVATE OmegaCommonCore)

add_test(NAME json_lookup COMMAND json-lookup-test)

add_executable(json-parse-result-test JSONParseResultTest.cpp)
set_target_properties(json-parse-result-test PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(json-parse-result-test OmegaCommonCore)
target_link_libraries(json-parse-result-test PRIVATE OmegaCommonCore)

add_test(NAME json_parse_result COMMAND json-parse-result-test)

add_executable(json-convert-test JSONConvertTest.cpp)
set_target_properties(json-convert-test PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(json-convert-test OmegaCommonCore)
target_link_libraries(json-convert-test PRIVATE OmegaCommonCore)

add_test(NAME json_convert COMMAND json-convert-test)

add_custom_target(common-core-tests
	COMMAND json-lifecycle-test
	COMMAND json-number-test
	COMMAND json-lookup-test
	COMMAND json-parse-result-test
	COMMAND json-convert-test
	DEPENDS json-lifecycle-test json-number-test json-lookup-test json-parse-result-test json-convert-test
	COMMENT "Running OmegaCommon core-runtime unit tests")
