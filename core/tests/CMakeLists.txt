# ---------------------------------------------------------------------------
# C++ unit tests for hikcam_core, built with Catch2 v3.
#
# Enabled from the parent via -DHIK_BUILD_TESTS=ON. Catch2 is fetched at
# configure time (needs network once, then it is cached in the build tree).
# The unit tier needs no camera; add -DHIK_ENABLE_HARDWARE_TESTS=ON to also
# build the tests that open and grab from a physically connected device.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# Provide catch_discover_tests().
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# No-camera unit tests (always built when HIK_BUILD_TESTS is ON).
set(HIK_TEST_SOURCES
    test_mvserror.cpp
    test_captureoptions.cpp
    test_imagewindow.cpp
    test_sdk_smoke.cpp
    test_roi_align.cpp
    test_log.cpp
    test_camera_helpers.cpp
    test_simcamera.cpp
    test_ringbuffer.cpp
    test_ringbuffer_stream.cpp
    test_ringbuffer_mt.cpp
    test_ringbuffer_shm.cpp
    test_camipc.cpp
    test_worker.cpp)

# Camera-dependent integration tests (opt-in).
if(HIK_ENABLE_HARDWARE_TESTS)
    list(APPEND HIK_TEST_SOURCES test_capture_hw.cpp)
endif()

add_executable(hikcam_core_tests ${HIK_TEST_SOURCES})
target_link_libraries(hikcam_core_tests PRIVATE
    ${HIK_CAM_CORE_TARGET_NAME}
    Catch2::Catch2WithMain)

# test_worker.cpp starts the real camera process.
add_dependencies(hikcam_core_tests uc2_cam_worker)
target_compile_definitions(hikcam_core_tests PRIVATE
    UC2_CAM_WORKER_PATH="$<TARGET_FILE:uc2_cam_worker>")

# Register each Catch2 TEST_CASE with CTest so `ctest` lists them individually.
#
# SKIP_RETURN_CODE 4: Catch2 exits with 4 when every test case in the run was
# skipped. The [hardware] tests do exactly that when no camera is connected
# (SKIP("no HikRobot/MVS camera connected") in test_capture_hw.cpp). Without
# this property CTest reads the non-zero exit code as a failure and the suite
# goes red on any machine without a camera attached.
catch_discover_tests(hikcam_core_tests
    PROPERTIES SKIP_RETURN_CODE 4)
