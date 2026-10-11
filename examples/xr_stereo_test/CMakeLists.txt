# xr_stereo_test — headless check of the OpenXR stereo protocol (see main.c).
add_executable(xr_stereo_test main.c)
target_link_libraries(xr_stereo_test PRIVATE aeron)
set_target_properties(xr_stereo_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
file(RELATIVE_PATH XR_STEREO_TEST_SHADER_RELATIVE_DIR
     "${CMAKE_BINARY_DIR}" "${AERON_SHADER_OUTPUT_DIR}")
target_compile_definitions(xr_stereo_test PRIVATE
    XR_STEREO_TEST_SHADER_RELATIVE_DIR="${XR_STEREO_TEST_SHADER_RELATIVE_DIR}")
