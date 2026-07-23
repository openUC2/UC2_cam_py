cmake_minimum_required(VERSION 3.15...3.30)
project(hikcamtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build the reusable C++ core, but not the standalone terminal app: for the
# Python wheel we only need the library to link into the extension module.
set(HIKCAMTEST_BUILD_APP OFF CACHE BOOL "" FORCE)

# option for building the Python extension module, default ON. The parent project can override this with -DHIKCAMTEST_BUILD_PYTHON=OFF to skip building the extension module
option(HIKCAMTEST_BUILD_PYTHON "Build the Python extension module" ON)
add_subdirectory(core)



########## Continue with the Python extension module if requested.

if (HIKCAMTEST_BUILD_PYTHON)
    
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

    pybind11_add_module(_hikcamtest bindings/hikcamtest_py.cpp)
    target_link_libraries(_hikcamtest PRIVATE hikcamtest_core)

    # Bake the MVS runtime library directory into the module's rpath so importing
    # the extension works without exporting LD_LIBRARY_PATH.
    set_target_properties(_hikcamtest PROPERTIES
        BUILD_RPATH   "${HIKCAMTEST_MVS_LIB_DIR}"
        INSTALL_RPATH "${HIKCAMTEST_MVS_LIB_DIR}")
endif()
# Install the extension next to the pure-Python package (see wheel.packages).
install(TARGETS _hikcamtest DESTINATION hikcamtest)
