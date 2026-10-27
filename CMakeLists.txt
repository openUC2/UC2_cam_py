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

    # rpath: the SDK of the build tree for the module in build/, the copy in the
    # package for the installed one, so neither needs LD_LIBRARY_PATH.
    set_target_properties(_hik PROPERTIES
        BUILD_RPATH   "${HIKCAMTEST_MVS_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/_mvs/lib/${HIKCAMTEST_MVS_ARCH_DIR}")

    # Install the extension next to the pure-Python package (see wheel.packages,
    # which maps python/uc2_cam_py -> the wheel's uc2_cam_py/ directory).
    install(TARGETS _hik DESTINATION uc2_cam_py)

    # The camera process goes next to it: the package finds it there and starts
    # one per camera (create_camera(..., separate_process=True)).
    set_target_properties(uc2_cam_worker PROPERTIES
        INSTALL_RPATH "$ORIGIN/_mvs/lib/${HIKCAMTEST_MVS_ARCH_DIR}")
    install(TARGETS uc2_cam_worker DESTINATION uc2_cam_py)

    # The SDK runtime goes into the package as uc2_cam_py/_mvs/ (the SDK's own
    # layout), so an installed uc2_cam_py needs neither External/hik nor /opt/MVS.
    # A wheel cannot hold symlinks: each one is installed as a copy under its own
    # name - the SDK loads its parts by these unversioned names - and the
    # versioned file it points to is left out.
    set(_mvs_lib "${HIKCAMTEST_MVS_LIB_DIR}")
    file(GLOB_RECURSE _mvs_files LIST_DIRECTORIES false RELATIVE "${_mvs_lib}" "${_mvs_lib}/*")
    set(_mvs_skip "")
    foreach(_rel IN LISTS _mvs_files)
        if(IS_SYMLINK "${_mvs_lib}/${_rel}")
            file(READ_SYMLINK "${_mvs_lib}/${_rel}" _target)
            get_filename_component(_dir "${_rel}" DIRECTORY)
            if(_target MATCHES "\\.so\\.[0-9.]+$")
                if(_dir)
                    list(APPEND _mvs_skip "${_dir}/${_target}")
                else()
                    list(APPEND _mvs_skip "${_target}")
                endif()
            endif()
        endif()
    endforeach()
    foreach(_rel IN LISTS _mvs_files)
        if(_rel IN_LIST _mvs_skip)
            continue()
        endif()
        get_filename_component(_dir "${_rel}" DIRECTORY)
        get_filename_component(_name "${_rel}" NAME)
        get_filename_component(_real "${_mvs_lib}/${_rel}" REALPATH)
        install(PROGRAMS "${_real}" DESTINATION "uc2_cam_py/_mvs/lib/${HIKCAMTEST_MVS_ARCH_DIR}/${_dir}"
                RENAME "${_name}")
    endforeach()
    if(HIKCAMTEST_MVS_ARCH_DIR STREQUAL "aarch64")
        set(_mvs_clp "Linux64_aarch64")
    else()
        set(_mvs_clp "Linux64_x64")
    endif()
    if(EXISTS "${HIKCAMTEST_MVS_ROOT}/lib/CLProtocol/${_mvs_clp}")
        install(DIRECTORY "${HIKCAMTEST_MVS_ROOT}/lib/CLProtocol/${_mvs_clp}"
                DESTINATION uc2_cam_py/_mvs/lib/CLProtocol)
    endif()
    install(DIRECTORY "${HIKCAMTEST_MVS_ROOT}/license" DESTINATION uc2_cam_py/_mvs OPTIONAL)
    install(FILES "${HIKCAMTEST_MVS_ROOT}/VERSION" DESTINATION uc2_cam_py/_mvs OPTIONAL)
endif()
