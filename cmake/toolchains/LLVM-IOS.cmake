set(CROSS_COMPILE TRUE)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_SYSTEM_NAME iOS)

set(CMAKE_IOS_INSTALL_COMBINED TRUE)

if(NOT IOS_MINIMUM_SUPPORT_VERSION)
    message("Must specify iOS support version in order to build for it." FATAL_ERROR)
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET ${IOS_MINIMUM_SUPPORT_VERSION})

# Every iOS device uses
set(CMAKE_OSX_ARCHITECTURES "armv7;armv7s;arm64;x86_64")