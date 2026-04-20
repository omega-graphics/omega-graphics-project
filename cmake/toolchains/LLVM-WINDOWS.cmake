include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "LLVM clang-cl C compiler"
    ERROR_MESSAGE "Unable to find clang-cl. Install LLVM for Windows or pass CMAKE_C_COMPILER explicitly."
    NAMES clang-cl
    HINTS "$ENV{ProgramFiles}/LLVM/bin"
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "LLVM clang-cl C++ compiler"
    ERROR_MESSAGE "Unable to find clang-cl. Install LLVM for Windows or pass CMAKE_CXX_COMPILER explicitly."
    NAMES clang-cl
    HINTS "$ENV{ProgramFiles}/LLVM/bin"
)
