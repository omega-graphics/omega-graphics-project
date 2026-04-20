include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

if(NOT DEFINED CMAKE_C_COMPILER OR "${CMAKE_C_COMPILER}" STREQUAL "")
    omega_find_xcrun_tool(_omega_macos_clang clang)
    if(_omega_macos_clang)
        set(CMAKE_C_COMPILER "${_omega_macos_clang}" CACHE FILEPATH "Apple Clang C compiler" FORCE)
    endif()
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL "")
    omega_find_xcrun_tool(_omega_macos_clangxx clang++)
    if(_omega_macos_clangxx)
        set(CMAKE_CXX_COMPILER "${_omega_macos_clangxx}" CACHE FILEPATH "Apple Clang C++ compiler" FORCE)
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
