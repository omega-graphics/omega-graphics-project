include("${CMAKE_CURRENT_LIST_DIR}/CompilerSelection.cmake")

omega_set_compiler(CMAKE_C_COMPILER CC
    DOC "MSVC C compiler"
    ERROR_MESSAGE "Unable to find cl.exe. Run CMake from a Visual Studio Developer Command Prompt or use a Visual Studio generator."
    NAMES cl
)

omega_set_compiler(CMAKE_CXX_COMPILER CXX
    DOC "MSVC C++ compiler"
    ERROR_MESSAGE "Unable to find cl.exe. Run CMake from a Visual Studio Developer Command Prompt or use a Visual Studio generator."
    NAMES cl
)
