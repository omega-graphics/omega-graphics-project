set(CROSS_COMPILE TRUE)

include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

set(CMAKE_SYSTEM_NAME Android)

if((NOT DEFINED CMAKE_C_COMPILER OR "${CMAKE_C_COMPILER}" STREQUAL "") OR
   (NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL ""))
    omega_find_android_llvm_bin(_omega_android_llvm_bin)
endif()

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "Android LLVM C compiler"
    ERROR_MESSAGE "Unable to find the Android clang compiler. Set CMAKE_ANDROID_NDK, ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT."
    NAMES clang
    PATHS "${_omega_android_llvm_bin}"
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "Android LLVM C++ compiler"
    ERROR_MESSAGE "Unable to find the Android clang++ compiler. Set CMAKE_ANDROID_NDK, ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT."
    NAMES clang++
    PATHS "${_omega_android_llvm_bin}"
)

find_program(ANDROID_SDK_TOOLS NAMES sdkmanager REQUIRED)

if(ANDROID_API_VERSION)
    set(CMAKE_SYSTEM_VERSION ${ANDROID_API_VERSION})
else()
    message(FATAL_ERROR "Android API version must be set in order for this to compile correctly. ANDROID_API_VERSION")
endif()
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
