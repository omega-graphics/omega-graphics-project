enable_testing()

# Backend-independent unit test for the std430 / std140 buffer-layout math
# (OmegaSL §2.4). Pure CPU — no GPU device needed, so it runs on any host
# (including Metal builds, where omegaSLStructStride itself never takes the
# std140 path). Exercises the BufferIO.h helpers directly.
add_executable(omegagte_std140_layout_test std140_layout_test.cpp)
target_include_directories(omegagte_std140_layout_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/include")
target_link_libraries(omegagte_std140_layout_test PRIVATE OmegaCommon)
add_test(NAME omegagte_std140_layout COMMAND omegagte_std140_layout_test)

if(TARGET_DIRECTX)
    add_subdirectory("directx")
elseif(TARGET_METAL AND NOT IOS)
    add_subdirectory("metal")
elseif(TARGET_VULKAN)
    add_subdirectory("vulkan")
endif()