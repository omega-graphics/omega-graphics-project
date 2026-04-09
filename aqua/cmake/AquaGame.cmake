# AQUA Game Build Function
#
# Usage:
#   add_aqua_game(
#       NAME        MyGame
#       BUNDLE_ID   "com.studio.mygame"
#       SOURCES     src/MyGame.cpp
#       [DEPS       ...]
#       [BUNDLE_ICON icon.icns]
#   )
#
# This creates a platform-appropriate application bundle:
#   macOS  — .app bundle (via add_app_bundle) with embedded AQUA + OmegaGTE frameworks
#   Win32  — executable with DPI manifest, copies AQUA + OmegaGTE DLLs alongside
#   Linux  — plain executable linked against AQUA
#
# Each platform has its own entry point in target/. Games provide Aqua::CreateApp().

set(AQUA_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

function(add_aqua_game)
    cmake_parse_arguments("_ARG" "" "NAME;BUNDLE_ID;BUNDLE_ICON" "SOURCES;DEPS" ${ARGN})

    if(TARGET_MACOS)
        # --- macOS: .app bundle with embedded frameworks ---
        set(BUNDLE_ID ${_ARG_BUNDLE_ID})
        set(APPNAME ${_ARG_NAME})
        set(BUNDLE_ICON ${_ARG_BUNDLE_ICON})
        configure_file(
            ${AQUA_SOURCE_DIR}/target/macos/Info.plist.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_Info.plist @ONLY)

        set(_RESOURCES)
        if(_ARG_BUNDLE_ICON)
            list(APPEND _RESOURCES ${_ARG_BUNDLE_ICON})
        endif()

        add_app_bundle(
            NAME ${_ARG_NAME}
            PLIST "${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_Info.plist"
            RESOURCES ${_RESOURCES}
            DEPS AQUA OmegaGTE.framework ${_ARG_DEPS}
            EMBEDDED_FRAMEWORKS OmegaGTE
            SOURCES ${_ARG_SOURCES} ${AQUA_SOURCE_DIR}/target/macos/main.mm)

        add_dependencies(${_ARG_NAME} AQUA)
        target_link_libraries(${_ARG_NAME} PRIVATE AQUA)
        target_link_frameworks(${_ARG_NAME} OmegaGTE)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${AQUA_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_MACOS TARGET_METAL)

    elseif(TARGET_WIN32)
        # --- Win32: executable with manifest + DLL copy ---
        set(EMBED "\"${AQUA_SOURCE_DIR}/target/win32/app.exe.manifest\"")
        configure_file(
            ${AQUA_SOURCE_DIR}/target/win32/manifest.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_res.rc)

        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            ${AQUA_SOURCE_DIR}/target/win32/mmain.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}_res.rc)

        target_link_libraries(${_ARG_NAME} PRIVATE AQUA)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${AQUA_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_WIN32 TARGET_DIRECTX)
        target_link_options(${_ARG_NAME} PRIVATE /SUBSYSTEM:CONSOLE /MANIFEST:NO)

        # Copy AQUA and GTE DLLs next to the executable
        add_custom_command(TARGET ${_ARG_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:AQUA> $<TARGET_FILE_DIR:${_ARG_NAME}>/$<TARGET_FILE_NAME:AQUA>
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:OmegaGTE> $<TARGET_FILE_DIR:${_ARG_NAME}>/$<TARGET_FILE_NAME:OmegaGTE>
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:OmegaCommon> $<TARGET_FILE_DIR:${_ARG_NAME}>/$<TARGET_FILE_NAME:OmegaCommon>)

    else()
        # --- Linux: plain executable ---
        add_executable(${_ARG_NAME}
            ${_ARG_SOURCES}
            ${AQUA_SOURCE_DIR}/target/linux/main.cpp)

        target_link_libraries(${_ARG_NAME} PRIVATE AQUA)
        target_include_directories(${_ARG_NAME} PRIVATE
            ${AQUA_SOURCE_DIR}/include)
        target_compile_definitions(${_ARG_NAME} PRIVATE TARGET_VULKAN)
    endif()

    # Common deps
    foreach(_dep ${_ARG_DEPS})
        add_dependencies(${_ARG_NAME} ${_dep})
    endforeach()
endfunction()
