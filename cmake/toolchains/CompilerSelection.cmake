include_guard(GLOBAL)

function(omega_set_compiler CacheVar EnvVar)
    cmake_parse_arguments(ARG "" "DOC;ERROR_MESSAGE" "NAMES;HINTS;PATHS" ${ARGN})

    if(DEFINED ${CacheVar} AND NOT "${${CacheVar}}" STREQUAL "")
        return()
    endif()

    if(DEFINED ENV{${EnvVar}} AND NOT "$ENV{${EnvVar}}" STREQUAL "")
        set(_omega_env_compiler "$ENV{${EnvVar}}")
        if(IS_ABSOLUTE "${_omega_env_compiler}" OR _omega_env_compiler MATCHES "[/\\\\]")
            set(${CacheVar} "${_omega_env_compiler}" CACHE FILEPATH "${ARG_DOC}" FORCE)
        else()
            set(${CacheVar} "${_omega_env_compiler}" CACHE STRING "${ARG_DOC}" FORCE)
        endif()
        return()
    endif()

    unset(_omega_compiler CACHE)
    unset(_omega_compiler)

    find_program(_omega_compiler
        NAMES ${ARG_NAMES}
        HINTS ${ARG_HINTS}
        PATHS ${ARG_PATHS}
    )

    if(NOT _omega_compiler)
        if(ARG_ERROR_MESSAGE)
            message(FATAL_ERROR "${ARG_ERROR_MESSAGE}")
        else()
            message(FATAL_ERROR "Failed to find a compiler for ${CacheVar}.")
        endif()
    endif()

    set(${CacheVar} "${_omega_compiler}" CACHE FILEPATH "${ARG_DOC}" FORCE)
endfunction()

function(omega_find_xcrun_tool OutVar ToolName)
    find_program(_omega_xcrun xcrun)
    if(NOT _omega_xcrun)
        unset(${OutVar} PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND "${_omega_xcrun}" --find "${ToolName}"
        RESULT_VARIABLE _omega_xcrun_status
        OUTPUT_VARIABLE _omega_xcrun_tool
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(_omega_xcrun_status EQUAL 0 AND NOT "${_omega_xcrun_tool}" STREQUAL "")
        set(${OutVar} "${_omega_xcrun_tool}" PARENT_SCOPE)
        return()
    endif()

    unset(${OutVar} PARENT_SCOPE)
endfunction()

function(omega_find_android_llvm_bin OutVar)
    set(_omega_ndk_roots)

    foreach(_omega_ndk_var
        CMAKE_ANDROID_NDK
        ANDROID_NDK
    )
        if(DEFINED ${_omega_ndk_var} AND NOT "${${_omega_ndk_var}}" STREQUAL "")
            list(APPEND _omega_ndk_roots "${${_omega_ndk_var}}")
        endif()
    endforeach()

    foreach(_omega_ndk_env
        ANDROID_NDK
        ANDROID_NDK_HOME
        ANDROID_NDK_ROOT
    )
        if(DEFINED ENV{${_omega_ndk_env}} AND NOT "$ENV{${_omega_ndk_env}}" STREQUAL "")
            list(APPEND _omega_ndk_roots "$ENV{${_omega_ndk_env}}")
        endif()
    endforeach()

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(_omega_host_tags darwin-arm64 darwin-x86_64)
        else()
            set(_omega_host_tags darwin-x86_64 darwin-arm64)
        endif()
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_omega_host_tags windows-x86_64)
    else()
        set(_omega_host_tags linux-x86_64)
    endif()

    foreach(_omega_ndk_root IN LISTS _omega_ndk_roots)
        foreach(_omega_host_tag IN LISTS _omega_host_tags)
            set(_omega_candidate_bin "${_omega_ndk_root}/toolchains/llvm/prebuilt/${_omega_host_tag}/bin")
            if(EXISTS "${_omega_candidate_bin}/clang" OR EXISTS "${_omega_candidate_bin}/clang.exe")
                if(NOT DEFINED CMAKE_ANDROID_NDK OR "${CMAKE_ANDROID_NDK}" STREQUAL "")
                    set(CMAKE_ANDROID_NDK "${_omega_ndk_root}" CACHE PATH "Android NDK root" FORCE)
                endif()
                set(${OutVar} "${_omega_candidate_bin}" PARENT_SCOPE)
                return()
            endif()
        endforeach()

        file(GLOB _omega_prebuilt_dirs LIST_DIRECTORIES TRUE "${_omega_ndk_root}/toolchains/llvm/prebuilt/*")
        list(SORT _omega_prebuilt_dirs)
        foreach(_omega_prebuilt_dir IN LISTS _omega_prebuilt_dirs)
            set(_omega_candidate_bin "${_omega_prebuilt_dir}/bin")
            if(EXISTS "${_omega_candidate_bin}/clang" OR EXISTS "${_omega_candidate_bin}/clang.exe")
                if(NOT DEFINED CMAKE_ANDROID_NDK OR "${CMAKE_ANDROID_NDK}" STREQUAL "")
                    set(CMAKE_ANDROID_NDK "${_omega_ndk_root}" CACHE PATH "Android NDK root" FORCE)
                endif()
                set(${OutVar} "${_omega_candidate_bin}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()

    message(FATAL_ERROR
        "Unable to find the Android NDK LLVM toolchain. Set CMAKE_ANDROID_NDK, "
        "ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT before using "
        "cmake/toolchains/LLVM-ANDROID.cmake."
    )
endfunction()
