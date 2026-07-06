cmake_minimum_required(VERSION 3.15...3.30)
project(hikcamtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build the reusable C++ core, but not the standalone terminal app: for the
# Python wheel we only need the library to link into the extension module.
set(HIKCAMTEST_BUILD_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(core)

# pybind11 is provided by the build environment (see pyproject.toml build deps).
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_hikcamtest bindings/hikcamtest_py.cpp)
target_link_libraries(_hikcamtest PRIVATE hikcamtest_core)

# Bake the MVS runtime library directory into the module's rpath so importing
# the extension works without exporting LD_LIBRARY_PATH.
set_target_properties(_hikcamtest PROPERTIES
    BUILD_RPATH   "${HIKCAMTEST_MVS_LIB_DIR}"
    INSTALL_RPATH "${HIKCAMTEST_MVS_LIB_DIR}")

# Install the extension next to the pure-Python package (see wheel.packages).
install(TARGETS _hikcamtest DESTINATION hikcamtest)
