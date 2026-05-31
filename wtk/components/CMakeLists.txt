# wtk/components/CMakeLists.txt
#
# Umbrella for OmegaWTK opt-in components. Each subdirectory that ships its
# own CMakeLists.txt declares one component library (target name
# `OmegaWTKComponent_<Name>`, STATIC, PIC ON, linked into consumer apps).
#
# Discovery is automatic: every subdirectory of `wtk/components/` that has a
# `CMakeLists.txt` is included here. Per-component opt-in is the subdir's
# own concern — see `gteview/CMakeLists.txt` for the canonical shape.
#
# Adding a new component:
#   1. Create `wtk/components/<name>/CMakeLists.txt` following the gteview
#      pattern (component option flag, public include dir, STATIC target).
#   2. Public headers live under
#      `wtk/components/<name>/include/omegaWTK/Components/<Name>/`.
#   3. Implementation lives under `wtk/components/<name>/src/`.
#   4. Consumers add `target_link_libraries(<app> PRIVATE
#      OmegaWTKComponent_<Name>)` after the `OmegaWTKApp(...)` call to pull
#      the component in. `OmegaWTKApp` does NOT auto-link components — that
#      is intentional, so component selection is explicit per consumer.
#
# This file is only reached when `OMEGAWTK_BUILD_COMPONENTS=ON`, gated from
# the parent `wtk/CMakeLists.txt`.

file(GLOB _component_dirs LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(_dir ${_component_dirs})
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
        add_subdirectory(${_dir})
    endif()
endforeach()
