include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(MediaCodecTest MediaCodecTest.cpp)
target_link_libraries(MediaCodecTest PRIVATE OmegaCommonCore OmegaCommonImg GTest::gtest GTest::gtest_main)

# Win32: the test exe needs OmegaCommonCore.dll + OmegaCommonImg.dll + ICU + libxml2 next to itself
# to load. On Linux/macOS the rpath baked in at link time finds them.
omega_stage_runtime_dlls(MediaCodecTest)

include(GoogleTest)
gtest_discover_tests(MediaCodecTest)
