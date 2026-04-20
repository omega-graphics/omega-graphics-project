set(CROSS_COMPILE TRUE)

include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

if(NOT DEFINED CMAKE_C_COMPILER OR "${CMAKE_C_COMPILER}" STREQUAL "")
    omega_find_xcrun_tool(_omega_ios_clang clang)
    if(_omega_ios_clang)
        set(CMAKE_C_COMPILER "${_omega_ios_clang}" CACHE FILEPATH "Apple Clang C compiler" FORCE)
    endif()
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL "")
    omega_find_xcrun_tool(_omega_ios_clangxx clang++)
    if(_omega_ios_clangxx)
        set(CMAKE_CXX_COMPILER "${_omega_ios_clangxx}" CACHE FILEPATH "Apple Clang C++ compiler" FORCE)
    endif()
endif()

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "Apple Clang C compiler"
    ERROR_MESSAGE "Unable to find Apple clang. Install Xcode Command Line Tools or pass CMAKE_C_COMPILER explicitly."
    NAMES clang clang-20 clang-19 clang-18 clang-17
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "Apple Clang C++ compiler"
    ERROR_MESSAGE "Unable to find Apple clang++. Install Xcode Command Line Tools or pass CMAKE_CXX_COMPILER explicitly."
    NAMES clang++ clang++-20 clang++-19 clang++-18 clang++-17
)

set(CMAKE_SYSTEM_NAME iOS)

set(CMAKE_IOS_INSTALL_COMBINED TRUE)

if(NOT IOS_MINIMUM_SUPPORT_VERSION)
    message(FATAL_ERROR "Must specify iOS support version in order to build for it.")
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET ${IOS_MINIMUM_SUPPORT_VERSION})

# Every iOS device uses
set(CMAKE_OSX_ARCHITECTURES "armv7;armv7s;arm64;x86_64")
