cmake_minimum_required(VERSION 3.15)
project(HikCamtestCore LANGUAGES CXX)

# When built stand-alone, pick sensible defaults. When added via the top-level
# project (add_subdirectory) these are already set by the parent.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HIKCAMTEST_BUILD_APP "Build the standalone terminal application" ON)

# ---------------------------------------------------------------------------
# Locate the MVS / HikRobot Machine Vision SDK.
# Override with -DMVS_ROOT=/path or the MVCAM_SDK_PATH environment variable.
# ---------------------------------------------------------------------------
if(DEFINED ENV{MVCAM_SDK_PATH} AND NOT MVS_ROOT)
    set(MVS_ROOT "$ENV{MVCAM_SDK_PATH}")
endif()
if(NOT MVS_ROOT)
    set(MVS_ROOT "/opt/MVS")
endif()
# Alternative here: check env variables
message(STATUS "Using MVS SDK at: ${MVS_ROOT}")

set(MVS_INCLUDE_DIR "${MVS_ROOT}/include")
if(NOT EXISTS "${MVS_INCLUDE_DIR}/MvCameraControl.h")
    message(FATAL_ERROR
        "MvCameraControl.h not found under ${MVS_INCLUDE_DIR}.\n"
        "Set -DMVS_ROOT=/path/to/MVS (the directory that contains include/ and lib/).")
endif()

# 64-bit or 32-bit library subdirectory.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MVS_LIB_DIR "${MVS_ROOT}/lib/64")
else()
    set(MVS_LIB_DIR "${MVS_ROOT}/lib/32")
endif()

find_library(MVS_CAMERA_CONTROL_LIB
    NAMES MvCameraControl
    PATHS "${MVS_LIB_DIR}"
    NO_DEFAULT_PATH)
if(NOT MVS_CAMERA_CONTROL_LIB)
    message(FATAL_ERROR "libMvCameraControl not found in ${MVS_LIB_DIR}.")
endif()
message(STATUS "Using MVS library:  ${MVS_CAMERA_CONTROL_LIB}")

# Publish the library directory so the parent project can bake it into the
# rpath of the Python extension module.
set(HIKCAMTEST_MVS_LIB_DIR "${MVS_LIB_DIR}" CACHE INTERNAL "MVS runtime library dir")

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
add_library(hikcamtest_core STATIC
    src/MvsCamera.cpp
    src/ImageWindow.cpp
    src/MvsError.cpp
    src/CaptureApp.cpp)

set_target_properties(hikcamtest_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The include dirs and the MVS import lib are part of the public interface so
# consumers (CLI, Python module) inherit them.
target_include_directories(hikcamtest_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MVS_INCLUDE_DIR}")

target_link_libraries(hikcamtest_core PUBLIC
    "${MVS_CAMERA_CONTROL_LIB}"
    Threads::Threads
    ${CMAKE_DL_LIBS})

# X11 is an implementation detail (guarded inside ImageWindow.cpp); the define
# and headers are private, but the library must be linked by consumers.
if(X11_FOUND)
    target_compile_definitions(hikcamtest_core PRIVATE HAVE_X11)
    target_include_directories(hikcamtest_core PRIVATE "${X11_INCLUDE_DIR}")
    target_link_libraries(hikcamtest_core PUBLIC ${X11_LIBRARIES})
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(hikcamtest_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Standalone terminal application (optional; skipped during the Python build).
# ---------------------------------------------------------------------------
if(HIKCAMTEST_BUILD_APP)
    add_executable(hikcamtest src/main.cpp)
    target_link_libraries(hikcamtest PRIVATE hikcamtest_core)

    # Find libMvCameraControl (and its siblings) at runtime without needing
    # MVCAM_COMMON_RUNENV / LD_LIBRARY_PATH to be exported.
    set_target_properties(hikcamtest PROPERTIES
        BUILD_RPATH   "${MVS_LIB_DIR}"
        INSTALL_RPATH "${MVS_LIB_DIR}")

    install(TARGETS hikcamtest RUNTIME DESTINATION bin)
endif()
