cmake_minimum_required(VERSION 3.15...3.30)
project(uc2_cam_py LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# option for building the Python extension module, default ON. The parent project can override this with -DHIK_BUILD_PYTHON=OFF to skip building the extension module
option(HIK_BUILD_PYTHON "Build the Python extension module" ON)

# Example programs (Examples/: HikStandAlone CLI, RingBuffExample). ON for the
# IDE; pyproject.toml switches it OFF so the wheel only carries the module.
option(HIK_BUILD_EXAMPLES "Build the example programs" ON)

# CTest has to be enabled here, in the top-level directory: CMake only writes a
# CTestTestfile.cmake into binary directories whose source directory has testing
# enabled. With enable_testing() living only in core/CMakeLists.txt we would get
# build/core/CTestTestfile.cmake but not build/CTestTestfile.cmake -- and the
# latter is the file `ctest --test-dir build` and the VS Code CMake Tools
# extension look for. Called unconditionally so the file exists even when
# HIK_BUILD_TESTS is OFF (the test list is then simply empty); the directory
# property is inherited by every subdirectory.
enable_testing()

# Sanitizer / profiling switches (HIK_SANITIZERS, HIK_PROFILING). Included here,
# before any subdirectory, so the flags reach the core, the examples and the
# tests alike. See Docs/Tooling.md.
include("${CMAKE_CURRENT_SOURCE_DIR}/core/cmake/HikTooling.cmake")

# A sanitized extension module would need the sanitizer runtime LD_PRELOADed
# into Python; the analysis builds cover the C++ side only.
if(HIK_SANITIZERS AND HIK_BUILD_PYTHON)
    message(STATUS "HIK: HIK_SANITIZERS is set -> not building the Python module")
    set(HIK_BUILD_PYTHON OFF)
endif()

add_subdirectory(core)

if(HIK_BUILD_EXAMPLES)
    add_subdirectory(Examples)
endif()


########## Continue with the Python extension module if requested.

if (HIK_BUILD_PYTHON)
    
    # pybind11 is provided by the build environment (see pyproject.toml build deps).
    set(PYBIND11_FINDPYTHON ON)
   
    # scikit-build-core puts pybind11 on CMAKE_PREFIX_PATH by itself; a plain
    # IDE configure does not, so ask the interpreter where its pybind11 lives.
    if(NOT DEFINED SKBUILD AND NOT DEFINED pybind11_DIR)
        find_package(Python 3.12 COMPONENTS Interpreter QUIET)
        if(Python_Interpreter_FOUND)
            execute_process(
                COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                OUTPUT_VARIABLE _pybind11_cmakedir
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _pybind11_probe
                ERROR_QUIET)
            if(_pybind11_probe EQUAL 0)
                message(STATUS "pybind11 cmake dir: ${_pybind11_cmakedir}")
                list(APPEND CMAKE_PREFIX_PATH "${_pybind11_cmakedir}")
            else()
                message(STATUS "Interpreter ${Python_EXECUTABLE} has no pybind11")
            endif()
        endif()
    endif()
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(_hik bindings/hik_py.cpp)
    target_link_libraries(_hik PRIVATE hikcam_core)

    # Bake the MVS runtime library directory into the module's rpath so importing
    # the extension works without exporting LD_LIBRARY_PATH.
    set_target_properties(_hik PROPERTIES
        BUILD_RPATH   "${HIKCAMTEST_MVS_LIB_DIR}"
        INSTALL_RPATH "${HIKCAMTEST_MVS_LIB_DIR}")

    # Install the extension next to the pure-Python package (see wheel.packages,
    # which maps python/uc2_cam_py -> the wheel's uc2_cam_py/ directory).
    install(TARGETS _hik DESTINATION uc2_cam_py)
endif()
