enable_testing()

add_executable(omega-assetbundle-verifier AssetBundleVerifier.cpp)
set_target_properties(omega-assetbundle-verifier PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(omega-assetbundle-verifier OmegaCommonCore)
target_link_libraries(omega-assetbundle-verifier PRIVATE OmegaCommonCore)

set(ASSETC_TEST_COMMAND
	${PYEXEC} ${CMAKE_CURRENT_SOURCE_DIR}/RunTests.py
		--omega-assetc $<TARGET_FILE:omega-assetc>
		--assetbundle-verifier $<TARGET_FILE:omega-assetbundle-verifier>
		--suite-dir ${CMAKE_CURRENT_SOURCE_DIR}
		--asset-types ${CMAKE_CURRENT_SOURCE_DIR}/../AssetTypes.json)

add_test(NAME assetc_integration COMMAND ${ASSETC_TEST_COMMAND})

add_custom_target(assetc-tests
	COMMAND ${ASSETC_TEST_COMMAND}
	DEPENDS omega-assetc omega-assetbundle-verifier
	COMMENT "Running omega-assetc integration tests")
