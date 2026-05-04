set(CROSS_COMPILE TRUE)

include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

set(CMAKE_SYSTEM_NAME Android)

# If no NDK is configured, look for one fetched by AUTOMDEPS at <repo>/deps/android-ndk.
get_filename_component(_omega_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if((NOT DEFINED CMAKE_ANDROID_NDK OR "${CMAKE_ANDROID_NDK}" STREQUAL "") AND
   (NOT DEFINED ENV{ANDROID_NDK} OR "$ENV{ANDROID_NDK}" STREQUAL "") AND
   (NOT DEFINED ENV{ANDROID_NDK_HOME} OR "$ENV{ANDROID_NDK_HOME}" STREQUAL "") AND
   (NOT DEFINED ENV{ANDROID_NDK_ROOT} OR "$ENV{ANDROID_NDK_ROOT}" STREQUAL ""))
    set(_omega_default_ndk "${_omega_repo_root}/deps/android-ndk")
    if(IS_DIRECTORY "${_omega_default_ndk}")
        set(CMAKE_ANDROID_NDK "${_omega_default_ndk}" CACHE PATH "Android NDK root" FORCE)
    endif()
endif()

if((NOT DEFINED CMAKE_C_COMPILER OR "${CMAKE_C_COMPILER}" STREQUAL "") OR
   (NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL ""))
    omega_find_android_llvm_bin(_omega_android_llvm_bin)
endif()

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "Android LLVM C compiler"
    ERROR_MESSAGE "Unable to find the Android clang compiler. Set CMAKE_ANDROID_NDK, ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT, or run `autom-deps --target android` from the repo root to fetch the NDK."
    NAMES clang
    PATHS "${_omega_android_llvm_bin}"
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "Android LLVM C++ compiler"
    ERROR_MESSAGE "Unable to find the Android clang++ compiler. Set CMAKE_ANDROID_NDK, ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT, or run `autom-deps --target android` from the repo root to fetch the NDK."
    NAMES clang++
    PATHS "${_omega_android_llvm_bin}"
)

# sdkmanager is only needed for packaging .apk artifacts. Compiling the
# native shared library only needs the NDK, so this is best-effort.
find_program(ANDROID_SDK_TOOLS NAMES sdkmanager)

if(NOT DEFINED ANDROID_API_VERSION OR "${ANDROID_API_VERSION}" STREQUAL "")
    set(ANDROID_API_VERSION 24 CACHE STRING "Android API level" FORCE)
endif()
set(CMAKE_SYSTEM_VERSION ${ANDROID_API_VERSION})

if(NOT DEFINED CMAKE_ANDROID_ARCH_ABI OR "${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "")
    set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
endif()
