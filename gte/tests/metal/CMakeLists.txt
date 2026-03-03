include(CMakeParseArguments)

function(add_metal_test)
    cmake_parse_arguments("_ARG" "" "NAME;PLIST" "SOURCES;RESOURCES;FRAMEWORKS;LIBS" ${ARGN})
    get_target_property(OMEGA_GTE_LIB "OmegaGTE" RUNTIME_OUTPUT_DIRECTORY)
    set(OMEGA_GTE_LIB "${OMEGA_GTE_LIB}/OmegaGTE.framework")
    message("${OMEGA_GTE_LIB}")
    add_executable(${_ARG_NAME} MACOSX_BUNDLE ${_ARG_SOURCES} ${_ARG_RESOURCES})
    add_dependencies(${_ARG_NAME} "OmegaGTE")
    
    set(_FRAMEWORKS)
    foreach(f ${_ARG_FRAMEWORKS})
        find_library("__${f}" ${f})
        list(APPEND _FRAMEWORKS "${__${f}}")
    endforeach()

    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${_FRAMEWORKS})

    set_target_properties(${_ARG_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${_ARG_PLIST}
        RESOURCE "${_ARG_RESOURCES}"
        XCODE_EMBED_FRAMEWORKS "OmegaGTE"
        BUILD_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5;@executable_path/../Frameworks"
    )
    target_link_options(${_ARG_NAME} PRIVATE
        "-Wl,-rpath,@executable_path/../Frameworks/OmegaGTE.framework/Versions/0.5"
        "-Wl,-rpath,@executable_path/../Frameworks"
    )

    # Embed OmegaGTE.framework and OmegaCommon so the app bundle is self-contained (Ninja/Unix make; Xcode uses XCODE_EMBED_FRAMEWORKS)
    set(_BUNDLE_CONTENTS "${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}.app/Contents")
    set(_BUNDLE_FRAMEWORKS "${_BUNDLE_CONTENTS}/Frameworks")
    add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_BUNDLE_FRAMEWORKS}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/Frameworks/OmegaGTE.framework" "${_BUNDLE_FRAMEWORKS}/OmegaGTE.framework"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/lib/libOmegaCommon.dylib" "${_BUNDLE_FRAMEWORKS}/libOmegaCommon.dylib"
        COMMENT "Embedding OmegaGTE.framework and OmegaCommon into ${_ARG_NAME}.app"
    )
endfunction()

function(add_metal_cli_test)
    cmake_parse_arguments("_ARG" "" "NAME" "SOURCES;FRAMEWORKS;LIBS" ${ARGN})
    add_executable(${_ARG_NAME} ${_ARG_SOURCES})
    add_dependencies(${_ARG_NAME} "OmegaGTE")

    set(_FRAMEWORKS)
    foreach(f ${_ARG_FRAMEWORKS})
        find_library("__cli_${f}" ${f})
        list(APPEND _FRAMEWORKS "${__cli_${f}}")
    endforeach()

    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" ${_ARG_LIBS} ${_FRAMEWORKS})
    target_link_options(${_ARG_NAME} PRIVATE
        "-Wl,-rpath,${CMAKE_BINARY_DIR}/Frameworks/OmegaGTE.framework/Versions/0.5"
        "-Wl,-rpath,${CMAKE_BINARY_DIR}/lib"
    )
endfunction()

add_omegasl_lib("2DTestShaders" "${CMAKE_CURRENT_SOURCE_DIR}/2DTest/shaders.omegasl" "${CMAKE_CURRENT_BINARY_DIR}/shaders.omegasllib")


add_metal_test(
    NAME 
    "2DTest"
    SOURCES
    "2DTest/main.mm"
    RESOURCES
    "2DTest/English.lproj/MainMenu.xib"
    PLIST 
    "${CMAKE_CURRENT_SOURCE_DIR}/2DTest/Info.plist"
    FRAMEWORKS 
    "Cocoa"
    "Metal"
    "QuartzCore"
)

add_metal_cli_test(
    NAME
    "ComputeTest"
    SOURCES
    "ComputeTest/main.mm"
    FRAMEWORKS
    "Metal"
    "Foundation"
)

add_metal_cli_test(
    NAME
    "GPUTessTest"
    SOURCES
    "GPUTessTest/main.mm"
    FRAMEWORKS
    "Metal"
    "QuartzCore"
    "AppKit"
)
