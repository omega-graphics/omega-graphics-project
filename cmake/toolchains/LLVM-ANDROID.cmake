set(CROSS_COMPILE TRUE)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_SYSTEM_NAME Android)

if(ANDROID_API_VERSION)
    set(CMAKE_SYSTEM_VERSION ${ANDROID_API_VERSION})
else()
    message(FATAL_ERROR "Android API version must be set in order for this to compile correctly. ANDROID_API_VERSION")
endif()
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK )