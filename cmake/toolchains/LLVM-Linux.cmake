include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "LLVM C compiler"
    ERROR_MESSAGE "Unable to find clang. Install LLVM or pass CMAKE_C_COMPILER explicitly."
    NAMES clang clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 clang-14
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "LLVM C++ compiler"
    ERROR_MESSAGE "Unable to find clang++. Install LLVM or pass CMAKE_CXX_COMPILER explicitly."
    NAMES clang++ clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15 clang++-14
)
