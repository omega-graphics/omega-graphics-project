# StageRuntimeSharedLibs.cmake — copy `lib*.so*` files (preserving
# symlink chains) from SRC_DIR into DST_DIR. Invoked from a POST_BUILD
# step via `cmake -P` so the glob runs at build time, after the source
# tree has been populated by an ExternalProject install.
#
# Linux-only — Windows / macOS use platform-specific staging
# mechanisms (PE-side-by-side and Mach-O rpath bundles respectively),
# both already covered by `omega_stage_runtime_dlls` and the framework
# bundle's `Libraries` directory in `OmegaGraphicsSuite.cmake`.
#
# Used by:
#   OmegaWTK on Linux — to colocate FFmpeg + alsa-lib shared objects
#   alongside `libOmegaWTK.so` in `${CMAKE_BINARY_DIR}/lib` so the
#   loader resolves DT_NEEDED entries via the module's `$ORIGIN` rpath
#   without needing LD_LIBRARY_PATH.
#
# Required arguments (set via `-D` on the cmake -P command line):
#   SRC_DIR — absolute path to the dependency's install lib dir
#             (e.g. `${FFMPEG_INSTALL_DIR}/lib`).
#   DST_DIR — absolute path to stage into (typically
#             `${CMAKE_BINARY_DIR}/lib`).

if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
    message(FATAL_ERROR "StageRuntimeSharedLibs: SRC_DIR and DST_DIR must be set via -D.")
endif()

if(NOT EXISTS "${SRC_DIR}")
    # The dependency build hasn't produced its install yet — let the
    # post-build step succeed quietly so a stale ninja invocation
    # doesn't fail before the third-party project re-runs. The next
    # post-build will pick the files up.
    message(STATUS "StageRuntimeSharedLibs: SRC_DIR ${SRC_DIR} missing, skipping.")
    return()
endif()

file(MAKE_DIRECTORY "${DST_DIR}")

# Match every shared object plus its version chain symlinks
# (libfoo.so, libfoo.so.N, libfoo.so.N.M.P). The glob picks up
# both regular files and symlinks because IS_SYMLINK does not
# follow the link, so we keep the original artifact's type.
file(GLOB _entries
    "${SRC_DIR}/*.so"
    "${SRC_DIR}/*.so.*")

foreach(_entry IN LISTS _entries)
    get_filename_component(_name "${_entry}" NAME)
    set(_dest "${DST_DIR}/${_name}")

    if(IS_SYMLINK "${_entry}")
        # Preserve the symlink as-is. The target may be relative
        # (`libfoo.so -> libfoo.so.N.M.P`) — recreate verbatim so the
        # next link in the chain resolves inside DST_DIR rather than
        # pointing back into SRC_DIR.
        file(READ_SYMLINK "${_entry}" _target)
        if(EXISTS "${_dest}" OR IS_SYMLINK "${_dest}")
            file(REMOVE "${_dest}")
        endif()
        file(CREATE_LINK "${_target}" "${_dest}" SYMBOLIC)
    else()
        # Concrete file — copy with `copy_if_different` semantics so
        # repeat builds skip unchanged artifacts.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_entry}" "${_dest}"
            RESULT_VARIABLE _copy_rc)
        if(NOT _copy_rc EQUAL 0)
            message(WARNING "StageRuntimeSharedLibs: copy of ${_entry} failed (rc=${_copy_rc}).")
        endif()
    endif()
endforeach()
