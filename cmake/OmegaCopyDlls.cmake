# Build-time helper invoked via `cmake -P` from omega_stage_runtime_dlls().
#
# Copies every *.dll from ${SRC} to ${DST} when the source is newer than the
# destination (or the destination is missing). Skips when ${SRC} resolves to
# the same absolute path as ${DST}, so a target whose own output directory
# is ${CMAKE_BINARY_DIR}/bin does not try to copy onto itself.
#
# Required arguments:
#   SRC  Source directory (typically ${CMAKE_BINARY_DIR}/bin).
#   DST  Destination directory (typically a consumer target's output dir).

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "OmegaCopyDlls.cmake: must be invoked with -DSRC=... -DDST=...")
endif()

get_filename_component(_src_abs "${SRC}" ABSOLUTE)
get_filename_component(_dst_abs "${DST}" ABSOLUTE)
if(_src_abs STREQUAL _dst_abs)
    return()
endif()

if(NOT IS_DIRECTORY "${_src_abs}")
    # bin/ may not exist on a first configure that hasn't built OmegaCommon yet;
    # treat that as a no-op rather than a hard error so the consumer's
    # POST_BUILD doesn't fail before its dependencies have run.
    return()
endif()

file(MAKE_DIRECTORY "${_dst_abs}")
file(GLOB _dlls "${_src_abs}/*.dll")
foreach(_dll IN LISTS _dlls)
    get_filename_component(_name "${_dll}" NAME)
    set(_target "${_dst_abs}/${_name}")
    if(NOT EXISTS "${_target}" OR "${_dll}" IS_NEWER_THAN "${_target}")
        file(COPY "${_dll}" DESTINATION "${_dst_abs}")
    endif()
endforeach()
