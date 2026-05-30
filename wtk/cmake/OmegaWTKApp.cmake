# OmegaWTK App Build Function

function(OmegaWTKApp)

    cmake_parse_arguments("_ARG" "" "NAME;BUNDLE_ID;ASSET_DIR;BUNDLE_ICON" "SOURCES;DEPS" ${ARGN})
    
    if(TARGET_MACOS)
        set(BUNDLE_ID ${_ARG_BUNDLE_ID})
        set(APPNAME ${_ARG_NAME})
        set(BUNDLE_ICON ${_ARG_BUNDLE_ICON})
        configure_file(${OMEGAWTK_SOURCE_DIR}/target/macos/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
        add_app_bundle(
            NAME ${_ARG_NAME}
            PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            RESOURCES ${OMEGAWTK_SOURCE_DIR}/target/macos/MainMenu.nib ${_ARG_BUNDLE_ICON} ${OMEGAWTK_COMPOSITOR_SHADER_LIB}
            DEPS OmegaWTK.framework OmegaGTE.framework ${_ARG_DEPS}
            EMBEDDED_FRAMEWORKS OmegaWTK OmegaGTE
            EMBEDDED_LIBS OmegaVA
            SOURCES ${_ARG_SOURCES})
        add_dependencies(${_ARG_NAME} OmegaWTK.framework OmegaWTKCompositorShaderLib)
        target_link_frameworks(${_ARG_NAME} OmegaGTE OmegaWTK)
        target_link_options(${_ARG_NAME} PRIVATE -rpath @loader_path/../Frameworks/OmegaWTK.framework/Libraries)
        target_link_libraries(${_ARG_NAME} PRIVATE OmegaVA)
    else()
        add_executable(${_ARG_NAME} ${_ARG_SOURCES})
        add_dependencies(${_ARG_NAME} "OmegaWTK" OmegaWTKCompositorShaderLib)
        target_link_libraries(${_ARG_NAME} PUBLIC OmegaWTK)
    endif()
    if(TARGET_MACOS)
        target_include_directories(${_ARG_NAME} PRIVATE "include" "gte/include" "gte/common/include" "${CMAKE_BINARY_DIR}/deps/icu/include")
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_MACOS TARGET_METAL)
        target_sources(${_ARG_NAME} PRIVATE ${OMEGAWTK_SOURCE_DIR}/target/macos/main.mm)
    elseif(TARGET_WIN32)
        target_include_directories(${_ARG_NAME} PRIVATE "include" "gte/include" "gte/common/include" "${CMAKE_BINARY_DIR}/deps/icu/include")
        target_compile_definitions(${_ARG_NAME} PRIVATE OMEGAWTK_APP WINDOWS_PRIVATE TARGET_WIN32 TARGET_DIRECTX)
        set(EMBED "\"${OMEGAWTK_SOURCE_DIR}/target/win32/app.exe.manifest\"")
        configure_file(${OMEGAWTK_SOURCE_DIR}/target/win32/manifest.rc.in ${CMAKE_CURRENT_BINARY_DIR}/res.rc)
        # res.rc embeds app.exe.manifest by path, so the RC compiler reads the
        # manifest at compile time but CMake cannot infer that from the generated
        # res.rc. Declare it as an explicit object dependency so editing
        # app.exe.manifest forces the resource (and thus the EXE's embedded
        # manifest) to rebuild; otherwise an incremental build silently keeps the
        # stale manifest baked into the binary.
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/res.rc PROPERTIES
            OBJECT_DEPENDS "${OMEGAWTK_SOURCE_DIR}/target/win32/app.exe.manifest")
        target_sources(${_ARG_NAME} PRIVATE ${OMEGAWTK_SOURCE_DIR}/target/win32/mmain.cpp ${CMAKE_CURRENT_BINARY_DIR}/res.rc)
        target_link_options(${_ARG_NAME} PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup /MANIFEST:NO)

        # OmegaCommon, OmegaWTK, OmegaGTE land in ${CMAKE_BINARY_DIR}/bin/
        # as their own RUNTIME_OUTPUT_DIRECTORY. Their shared third-party
        # DLLs (ICU via OmegaCommon, libxml2 via OmegaWTK) are staged into
        # the same dir as POST_BUILD steps on each module. Fan all of them
        # out into the app's output dir in one go:
        omega_stage_runtime_dlls(${_ARG_NAME})

        # The compositor shader lib is a build-time artifact (not a DLL),
        # ship it next to the exe.
        add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                ${OMEGAWTK_COMPOSITOR_SHADER_LIB}
                $<TARGET_FILE_DIR:${_ARG_NAME}>/compositor.omegasllib)
    elseif(TARGET_LINUX)
        target_include_directories(${_ARG_NAME} PRIVATE "include" "gte/include" "gte/common/include" "${CMAKE_BINARY_DIR}/deps/icu/include")
        target_compile_definitions(${_ARG_NAME} PRIVATE OMEGAWTK_APP TARGET_GTK TARGET_VULKAN)
        # libOmegaWTK.so pulls in ffmpeg, which drags in system libavformat/librsvg.
        # Those expect versioned libxml2 symbols (LIBXML2_2.4.30, etc.) that the
        # bundled libxml2.so does not expose. Defer indirect-library symbol
        # resolution to the runtime loader, which finds the system libxml2.
        target_link_options(${_ARG_NAME} PRIVATE "-Wl,--allow-shlib-undefined")
        target_sources(${_ARG_NAME} PRIVATE ${OMEGAWTK_SOURCE_DIR}/target/gtk/main.cpp)
        add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${OMEGAWTK_COMPOSITOR_SHADER_LIB} $<TARGET_FILE_DIR:${_ARG_NAME}>/compositor.omegasllib)
    endif()
endfunction()
