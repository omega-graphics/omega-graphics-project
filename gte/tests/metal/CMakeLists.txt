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

    set_target_properties(${_ARG_NAME} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${_ARG_PLIST} RESOURCE "${_ARG_RESOURCES}"  XCODE_EMBED_FRAMEWORKS "OmegaGTE")

   
    
   
    # target_sources(${_ARG_NAME} PRIVATE ${OMEGA_GTE_LIB})
    # set_source_files_properties(${OMEGA_GTE_LIB} PROPERTIES MACOSX_PACKAGE_LOCATION "Frameworks")
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
