cmake_minimum_required(VERSION 3.15)
project(HikCamCore LANGUAGES CXX)

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
# Locate the MVS / HikRobot Machine Vision SDK.
# It ships with the project in External/hik/MVS (filled by `uv run fetch_mvs`,
# see External/hik/README.md). An installation such as /opt/MVS and the
# MVCAM_SDK_PATH variable an MVS installer exports are deliberately not used:
# only -DMVS_ROOT=/path overrides the bundle, e.g. to try another SDK version.
# ---------------------------------------------------------------------------
get_filename_component(MVS_BUNDLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../External/hik/MVS" ABSOLUTE)
if(MVS_ROOT)
    set(_mvs_source "-DMVS_ROOT")
else()
    set(MVS_ROOT "${MVS_BUNDLE_ROOT}")
    set(_mvs_source "bundled")
endif()

set(MVS_INCLUDE_DIR "${MVS_ROOT}/include")
if(NOT EXISTS "${MVS_INCLUDE_DIR}/MvCameraControl.h")
    message(FATAL_ERROR
        "MVS SDK not found: ${MVS_INCLUDE_DIR}/MvCameraControl.h is missing.\n"
        "Put the HikRobot Linux packages into External/hik and run `uv run fetch_mvs` "
        "(see External/hik/README.md), or point -DMVS_ROOT at an SDK tree (include/, lib/<arch>/).")
endif()
set(MVS_VERSION "unknown")
if(EXISTS "${MVS_ROOT}/VERSION")
    file(STRINGS "${MVS_ROOT}/VERSION" _mvs_sdk_line REGEX "^sdk=")
    string(REPLACE "sdk=" "" MVS_VERSION "${_mvs_sdk_line}")
endif()

# Library subdirectory by processor: the bundle holds x86_64 (lib/64) and
# aarch64 (lib/aarch64) side by side, so the pointer size alone cannot decide.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mvs_cpu)
if(_mvs_cpu MATCHES "^(x86_64|amd64)$")
    set(MVS_ARCH_DIR "64")
elseif(_mvs_cpu MATCHES "^(aarch64|arm64)$")
    set(MVS_ARCH_DIR "aarch64")
else()
    message(FATAL_ERROR "No MVS libraries for processor '${CMAKE_SYSTEM_PROCESSOR}' (supported: x86_64, aarch64).")
endif()
set(MVS_LIB_DIR "${MVS_ROOT}/lib/${MVS_ARCH_DIR}")

# Searched afresh on every configure: the cache of an older build tree may still
# point into another SDK (e.g. /opt/MVS).
unset(MVS_CAMERA_CONTROL_LIB CACHE)
find_library(MVS_CAMERA_CONTROL_LIB NAMES MvCameraControl PATHS "${MVS_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT MVS_CAMERA_CONTROL_LIB)
    message(FATAL_ERROR "libMvCameraControl not found in ${MVS_LIB_DIR} - run `uv run fetch_mvs`.")
endif()
message(STATUS "Using MVS SDK ${MVS_VERSION} (${_mvs_source}): ${MVS_ROOT}")
message(STATUS "Using MVS library:  ${MVS_CAMERA_CONTROL_LIB}")

# Published for the parent project: the rpath of the build tree and the SDK
# files the Python package installs next to its extension module.
set(HIKCAMTEST_MVS_ROOT     "${MVS_ROOT}"     CACHE INTERNAL "MVS SDK root")
set(HIKCAMTEST_MVS_LIB_DIR  "${MVS_LIB_DIR}"  CACHE INTERNAL "MVS runtime library dir")
set(HIKCAMTEST_MVS_ARCH_DIR "${MVS_ARCH_DIR}" CACHE INTERNAL "MVS library sub-directory (64 | aarch64)")

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

add_library(${HIK_CAM_CORE_TARGET_NAME} STATIC
    src/MvsCamera.cpp
    src/ImageWindow.cpp
    src/MvsError.cpp
    src/CaptureApp.cpp
    src/UC2_RingBuff.cpp
    src/hikLog.cpp
    src/CameraBase.cpp
    src/HikCamera.cpp
    src/SimCamera.cpp
    src/SimPattern.cpp)

set_target_properties(${HIK_CAM_CORE_TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The include dirs and the MVS import lib are part of the public interface so
# consumers (CLI, Python module) inherit them.
target_include_directories(${HIK_CAM_CORE_TARGET_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MVS_INCLUDE_DIR}")

target_link_libraries(${HIK_CAM_CORE_TARGET_NAME} PUBLIC
    "${MVS_CAMERA_CONTROL_LIB}"
    Threads::Threads
    ${CMAKE_DL_LIBS})

# Every program linking the core gets DT_RPATH instead of DT_RUNPATH: it is
# searched before LD_LIBRARY_PATH, which an MVS installer points at /opt/MVS -
# the SDK this project was built against must win.
target_link_options(${HIK_CAM_CORE_TARGET_NAME} INTERFACE "LINKER:--disable-new-dtags")

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
# C++ unit tests (Catch2 v3). Off by default so wheel builds stay lean;
# enable with -DHIK_BUILD_TESTS=ON, then run with ctest.
# ---------------------------------------------------------------------------
if(HIK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
