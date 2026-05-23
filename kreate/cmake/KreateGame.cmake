# KREATE Game Build Function
#
# Usage:
#   add_kreate_game(
#       NAME        MyGame
#       BUNDLE_ID   "com.studio.mygame"
#       SOURCES     src/MyGame.cpp
#       [DEPS       ...]
#       [BUNDLE_ICON icon.icns]
#   )
#
# This creates a platform-appropriate application bundle:
#   macOS  — .app bundle (via add_app_bundle) with embedded KREATE + OmegaGTE frameworks
#   Win32  — executable with DPI manifest, copies KREATE + OmegaGTE DLLs alongside
#   Linux  — plain executable linked against KREATE
#
# Each platform has its own entry point in target/. Games provide Kreate::CreateApp().

set(KREATE_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

function(add_kreate_game)
    cmake_parse_arguments("_ARG" "" "NAME;BUNDLE_ID;BUNDLE_ICON" "SOURCES;DEPS" ${ARGN})

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # --- iOS: plain executable + Info.plist (compile-only target).
        # A real .app/codesign pipeline mirroring add_app_bundle is a follow-up.
        set(BUNDLE_ID ${_ARG_BUNDLE_ID})
        set(APPNAME ${_ARG_NAME})
        configure_file(
            ${KREATE_SOURCE_DIR}/target/ios/Info.plist.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_Info.plist @ONLY)

        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            ${KREATE_SOURCE_DIR}/target/ios/main.mm)

        set_target_properties(${_ARG_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Apps")

        target_link_libraries(${_ARG_NAME} PRIVATE KREATE)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${KREATE_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_IOS TARGET_METAL)
        target_link_system_frameworks(${_ARG_NAME} UIKit QuartzCore Metal Foundation)

    elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
        # --- Android: shared library + AndroidManifest.xml stub.
        # APK packaging is a follow-up; this just produces lib<NAME>.so and a
        # manifest a downstream Gradle/AGP project can consume.
        set(BUNDLE_ID ${_ARG_BUNDLE_ID})
        set(APPNAME ${_ARG_NAME})
        configure_file(
            ${KREATE_SOURCE_DIR}/target/android/AndroidManifest.xml.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_AndroidManifest.xml @ONLY)

        add_library(${_ARG_NAME} SHARED
            ${_ARG_SOURCES}
            ${KREATE_SOURCE_DIR}/target/android/main.cpp)

        set_target_properties(${_ARG_NAME} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Apps")

        target_link_libraries(${_ARG_NAME} PRIVATE KREATE android log)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${KREATE_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_ANDROID TARGET_VULKAN VULKAN_TARGET_ANDROID)

    elseif(APPLE)
        # --- macOS: .app bundle with embedded frameworks ---
        set(BUNDLE_ID ${_ARG_BUNDLE_ID})
        set(APPNAME ${_ARG_NAME})
        set(BUNDLE_ICON ${_ARG_BUNDLE_ICON})
        configure_file(
            ${KREATE_SOURCE_DIR}/target/macos/Info.plist.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_Info.plist @ONLY)

        set(_RESOURCES)
        if(_ARG_BUNDLE_ICON)
            list(APPEND _RESOURCES ${_ARG_BUNDLE_ICON})
        endif()

        add_app_bundle(
            NAME ${_ARG_NAME}
            PLIST "${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_Info.plist"
            RESOURCES ${_RESOURCES}
            DEPS KREATE OmegaGTE.framework ${_ARG_DEPS}
            EMBEDDED_FRAMEWORKS OmegaGTE
            SOURCES ${_ARG_SOURCES} ${KREATE_SOURCE_DIR}/target/macos/main.mm)

        add_dependencies(${_ARG_NAME} KREATE)
        target_link_libraries(${_ARG_NAME} PRIVATE KREATE)
        target_link_frameworks(${_ARG_NAME} OmegaGTE)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${KREATE_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_MACOS TARGET_METAL)

    elseif(WIN32)
        # --- Win32: executable with manifest + DLL copy ---
        set(EMBED "\"${KREATE_SOURCE_DIR}/target/win32/app.exe.manifest\"")
        configure_file(
            ${KREATE_SOURCE_DIR}/target/win32/manifest.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_res.rc)

        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            ${KREATE_SOURCE_DIR}/target/win32/mmain.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_res.rc)

        set_target_properties(${_ARG_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Apps")

        target_link_libraries(${_ARG_NAME} PRIVATE KREATE)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${KREATE_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_WIN32 TARGET_DIRECTX)
        target_link_options(${_ARG_NAME} PRIVATE /SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup /MANIFEST:NO)

        # KREATE, OmegaGTE, and OmegaCommon land in ${CMAKE_BINARY_DIR}/bin/
        # as their own RUNTIME_OUTPUT_DIRECTORY. OmegaCommon also stages
        # its shared third-party DLLs (ICU) into bin/ post-build. Fan all
        # of them out into Apps/ in one go:
        omega_stage_runtime_dlls(${_ARG_NAME})

    else()
        # --- Linux: plain executable ---
        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            ${KREATE_SOURCE_DIR}/target/linux/main.cpp)

        set_target_properties(${_ARG_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Apps")

        target_link_libraries(${_ARG_NAME} PRIVATE KREATE)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${KREATE_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_VULKAN)
    endif()

    # Common deps
    foreach(_dep ${_ARG_DEPS})
        add_dependencies(${_ARG_NAME} ${_dep})
    endforeach()
endfunction()
