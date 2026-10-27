cmake_minimum_required(VERSION 3.15)
# UC2_CAM_VERSION from the file VERSION in the repository root (a no-op when
# the top-level project already included it).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/UC2Version.cmake")
project(HikCamCore VERSION ${UC2_CAM_VERSION} LANGUAGES CXX)

# When built stand-alone, pick sensible defaults. When added via the top-level
# project (add_subdirectory) these are already set by the parent.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HIK_BUILD_TESTS "Build the C++ unit tests (Catch2)" OFF)
option(HIK_ENABLE_HARDWARE_TESTS "Also build C++ tests that need a connected camera" OFF)

# Sanitizer / profiling switches (HIK_SANITIZERS, HIK_PROFILING). A no-op when
# the top-level project already included it.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HikTooling.cmake")

# ---------------------------------------------------------------------------
# Locate the MVS / HikRobot Machine Vision SDK - optional.
# It ships with the project in External/hik/MVS (filled by `uv run fetch_mvs`,
# see External/hik/README.md). An installation such as /opt/MVS and the
# MVCAM_SDK_PATH variable an MVS installer exports are deliberately not used:
# only -DMVS_ROOT=/path overrides the bundle, e.g. to try another SDK version.
#
#   HIK_MVS  AUTO  use the SDK if it is there and fits, else build without it
#                  (default; also when -DMVS_ROOT points somewhere unusable)
#            ON    the SDK is required: missing or unfit is an error
#            OFF   build without it, do not even look
#
# "Fits" means the SDK users compile and the library links against it
# (cmake/MvsProbe.cmake). Without the SDK there is no HikRobot support: the
# Python package then offers only the simulated camera and logs an error
# naming HIK_MVS_MISSING_REASON. HIK_HAVE_MVS (ON/OFF) is the result; the code
# sees it as HIK_HAVE_MVS=1/0.
# ---------------------------------------------------------------------------
set(HIK_MVS "AUTO" CACHE STRING "HikRobot MVS SDK: AUTO (use it if found and fitting), ON (required) or OFF (without)")
set_property(CACHE HIK_MVS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${HIK_MVS}" _hik_mvs_mode)
if(NOT _hik_mvs_mode MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "HIK_MVS must be AUTO, ON or OFF, got '${HIK_MVS}'")
endif()

# `uv run fetch_mvs` adds External/hik/MVS later on: the next build then
# re-configures by itself and picks the SDK up.
get_filename_component(_hik_external "${CMAKE_CURRENT_LIST_DIR}/../External/hik" ABSOLUTE)
if(IS_DIRECTORY "${_hik_external}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hik_external}")
endif()

set(HIK_HAVE_MVS OFF)
set(_mvs_missing "")
if(_hik_mvs_mode STREQUAL "OFF")
    set(_mvs_missing "HIK_MVS=OFF")
    message(STATUS "HIK_MVS=OFF: building without HikRobot support (only the simulated camera)")
else()
    get_filename_component(MVS_BUNDLE_ROOT "${_hik_external}/MVS" ABSOLUTE)
    if(MVS_ROOT)
        set(_mvs_source "-DMVS_ROOT=${MVS_ROOT}")
        set(_mvs_hint
            "Point MVS_ROOT at a complete SDK that fits this code (include/, lib/<arch>/), or leave "
            "it unset to use External/hik/MVS (`uv run fetch_mvs`, see External/hik/README.md).")
    else()
        set(MVS_ROOT "${MVS_BUNDLE_ROOT}")
        set(_mvs_source "bundled")
        set(_mvs_hint
            "Put the HikRobot Linux packages into External/hik and run `uv run fetch_mvs` "
            "(see External/hik/README.md), or point -DMVS_ROOT at an SDK tree (include/, lib/<arch>/).")
    endif()

    set(MVS_INCLUDE_DIR "${MVS_ROOT}/include")
    # Library subdirectory by processor: the bundle holds x86_64 (lib/64) and
    # aarch64 (lib/aarch64) side by side, so the pointer size alone cannot decide.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mvs_cpu)
    if(NOT EXISTS "${MVS_INCLUDE_DIR}/MvCameraControl.h")
        set(_mvs_missing "${MVS_INCLUDE_DIR}/MvCameraControl.h is missing")
    elseif(_mvs_cpu MATCHES "^(x86_64|amd64)$")
        set(MVS_ARCH_DIR "64")
    elseif(_mvs_cpu MATCHES "^(aarch64|arm64)$")
        set(MVS_ARCH_DIR "aarch64")
    else()
        set(_mvs_missing "there are no MVS libraries for processor '${CMAKE_SYSTEM_PROCESSOR}' (supported: x86_64, aarch64)")
    endif()

    if(NOT _mvs_missing)
        set(MVS_LIB_DIR "${MVS_ROOT}/lib/${MVS_ARCH_DIR}")
        # Searched afresh on every configure: the cache of an older build tree may
        # still point into another SDK (e.g. /opt/MVS).
        unset(MVS_CAMERA_CONTROL_LIB CACHE)
        find_library(MVS_CAMERA_CONTROL_LIB NAMES MvCameraControl PATHS "${MVS_LIB_DIR}" NO_DEFAULT_PATH)
        if(NOT MVS_CAMERA_CONTROL_LIB)
            set(_mvs_missing "libMvCameraControl is missing in ${MVS_LIB_DIR}")
        endif()
    endif()

    if(NOT _mvs_missing)
        include("${CMAKE_CURRENT_LIST_DIR}/cmake/MvsProbe.cmake")
        hik_mvs_probe(_mvs_probe_error)
        if(_mvs_probe_error)
            set(_mvs_missing "the SDK in ${MVS_ROOT} does not fit this code: ${_mvs_probe_error}")
        endif()
    endif()

    if(_mvs_missing)
        if(_hik_mvs_mode STREQUAL "ON")
            message(FATAL_ERROR "MVS SDK (${_mvs_source}) not usable: ${_mvs_missing}.\n" ${_mvs_hint})
        endif()
        message(WARNING
            "MVS SDK (${_mvs_source}) not usable: ${_mvs_missing}.\n"
            "Building WITHOUT HikRobot support - only the simulated camera will be available.\n"
            ${_mvs_hint} " Then build again (re-install the Python package).")
    else()
        set(HIK_HAVE_MVS ON)
        set(MVS_VERSION "unknown")
        if(EXISTS "${MVS_ROOT}/VERSION")
            file(STRINGS "${MVS_ROOT}/VERSION" _mvs_sdk_line REGEX "^sdk=")
            string(REPLACE "sdk=" "" MVS_VERSION "${_mvs_sdk_line}")
        endif()
        message(STATUS "Using MVS SDK ${MVS_VERSION} (${_mvs_source}): ${MVS_ROOT}")
        message(STATUS "Using MVS library:  ${MVS_CAMERA_CONTROL_LIB}")
    endif()
endif()

# Published for the parent project: the rpath of the build tree and the SDK
# files the Python package installs next to its extension module. Emptied
# without the SDK, so no path of an earlier configure survives in the cache.
if(HIK_HAVE_MVS)
    set(HIKCAMTEST_MVS_ROOT     "${MVS_ROOT}"     CACHE INTERNAL "MVS SDK root")
    set(HIKCAMTEST_MVS_LIB_DIR  "${MVS_LIB_DIR}"  CACHE INTERNAL "MVS runtime library dir")
    set(HIKCAMTEST_MVS_ARCH_DIR "${MVS_ARCH_DIR}" CACHE INTERNAL "MVS library sub-directory (64 | aarch64)")
else()
    set(HIKCAMTEST_MVS_ROOT     "" CACHE INTERNAL "MVS SDK root")
    set(HIKCAMTEST_MVS_LIB_DIR  "" CACHE INTERNAL "MVS runtime library dir")
    set(HIKCAMTEST_MVS_ARCH_DIR "" CACHE INTERNAL "MVS library sub-directory (64 | aarch64)")
endif()
set(HIK_HAVE_MVS ${HIK_HAVE_MVS} CACHE INTERNAL "Built with the HikRobot MVS SDK")

# Why there is no SDK, for the error the Python package logs on import: one
# line, without the characters a C string literal or a CMake list would trip on.
string(REPLACE "\n" " " _mvs_reason "${_mvs_missing}")
string(REPLACE "\\" "/" _mvs_reason "${_mvs_reason}")
string(REPLACE "\"" "'" _mvs_reason "${_mvs_reason}")
string(REPLACE ";" "," _mvs_reason "${_mvs_reason}")
string(LENGTH "${_mvs_reason}" _mvs_reason_len)
if(_mvs_reason_len GREATER 400)
    string(SUBSTRING "${_mvs_reason}" 0 400 _mvs_reason)
    string(APPEND _mvs_reason "...")
endif()
set(HIK_MVS_MISSING_REASON "${_mvs_reason}" CACHE INTERNAL "Why the build has no MVS SDK (empty: it has one)")

# ---------------------------------------------------------------------------
# Optional X11 support for the live preview window.
# ---------------------------------------------------------------------------
find_package(X11)
if(X11_FOUND)
    message(STATUS "X11 found -> building with live preview window.")
else()
    message(WARNING
        "X11 development headers not found -> building HEADLESS (no preview window).\n"
        "  The tool will still grab frames and save a snapshot to disk.\n"
        "  To enable the window, install the X11 dev package and rebuild, e.g.:\n"
        "      sudo dnf install libX11-devel        # Fedora / Nobara\n"
        "      sudo apt install libx11-dev          # Debian / Ubuntu")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: all camera / capture logic, shared by the CLI and the Python
# binding. Built position-independent so it can be linked into a shared module.
# ---------------------------------------------------------------------------
set(HIK_CAM_CORE_TARGET_NAME "hikcam_core")

set(_hik_core_sources
    src/ImageWindow.cpp
    src/UC2_RingBuff.cpp
    src/hikLog.cpp
    src/CameraBase.cpp
    src/SimCamera.cpp
    src/SimPattern.cpp
    src/CamIpc.cpp
    src/CamWorker.cpp
    src/RemoteCamera.cpp
    src/Version.cpp)
if(HIK_HAVE_MVS)
    # Everything that talks to the SDK (MvCameraControl.h).
    list(APPEND _hik_core_sources
        src/MvsCamera.cpp
        src/MvsError.cpp
        src/CaptureApp.cpp
        src/HikCamera.cpp)
endif()
add_library(${HIK_CAM_CORE_TARGET_NAME} STATIC ${_hik_core_sources})

set_target_properties(${HIK_CAM_CORE_TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Only this one file sees the version number, so a new version recompiles just it.
set_source_files_properties(src/Version.cpp PROPERTIES
    COMPILE_DEFINITIONS "UC2_CAM_VERSION=\"${UC2_CAM_VERSION}\"")

# The include dirs and the MVS import lib are part of the public interface so
# consumers (CLI, Python module) inherit them, and so is the HIK_HAVE_MVS switch.
target_include_directories(${HIK_CAM_CORE_TARGET_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${HIK_CAM_CORE_TARGET_NAME} PUBLIC
    Threads::Threads
    ${CMAKE_DL_LIBS})
if(HIK_HAVE_MVS)
    target_compile_definitions(${HIK_CAM_CORE_TARGET_NAME} PUBLIC HIK_HAVE_MVS=1)
    target_include_directories(${HIK_CAM_CORE_TARGET_NAME} PUBLIC "${MVS_INCLUDE_DIR}")
    target_link_libraries(${HIK_CAM_CORE_TARGET_NAME} PUBLIC "${MVS_CAMERA_CONTROL_LIB}")
    # Every program linking the core gets DT_RPATH instead of DT_RUNPATH: it is
    # searched before LD_LIBRARY_PATH, which an MVS installer points at /opt/MVS -
    # the SDK this project was built against must win.
    target_link_options(${HIK_CAM_CORE_TARGET_NAME} INTERFACE "LINKER:--disable-new-dtags")
else()
    target_compile_definitions(${HIK_CAM_CORE_TARGET_NAME} PUBLIC HIK_HAVE_MVS=0)
endif()

# X11 is an implementation detail (guarded inside ImageWindow.cpp); the define
# and headers are private, but the library must be linked by consumers.
if(X11_FOUND)
    target_compile_definitions(${HIK_CAM_CORE_TARGET_NAME} PRIVATE HAVE_X11)
    target_include_directories(${HIK_CAM_CORE_TARGET_NAME} PRIVATE "${X11_INCLUDE_DIR}")
    target_link_libraries(${HIK_CAM_CORE_TARGET_NAME} PUBLIC ${X11_LIBRARIES})
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${HIK_CAM_CORE_TARGET_NAME} PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# The camera process (one per camera, started by RemoteCamera). Built in both
# configuration paths, so the C++ tests can start it; the top-level project
# installs it next to the Python extension module.
# ---------------------------------------------------------------------------
add_executable(uc2_cam_worker worker/uc2_cam_worker.cpp)
target_link_libraries(uc2_cam_worker PRIVATE ${HIK_CAM_CORE_TARGET_NAME})
if(HIK_HAVE_MVS)
    set_target_properties(uc2_cam_worker PROPERTIES BUILD_RPATH "${MVS_LIB_DIR}")
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(uc2_cam_worker PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# C++ unit tests (Catch2 v3). Off by default so wheel builds stay lean;
# enable with -DHIK_BUILD_TESTS=ON, then run with ctest.
# ---------------------------------------------------------------------------
if(HIK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
