include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "GCC C compiler"
    ERROR_MESSAGE "Unable to find gcc. Install GCC or pass CMAKE_C_COMPILER explicitly."
    NAMES gcc gcc-15 gcc-14 gcc-13 gcc-12 gcc-11 gcc-10 gcc-9 gcc-8 gcc-7
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "GCC C++ compiler"
    ERROR_MESSAGE "Unable to find g++. Install GCC or pass CMAKE_CXX_COMPILER explicitly."
    NAMES g++ g++-15 g++-14 g++-13 g++-12 g++-11 g++-10 g++-9 g++-8 g++-7
)
