# ---------------------------------------------------------------------------
# The version of uc2_cam_py, read from the file VERSION in the repository root:
# the only place where the number is written down (README "Versioning").
#
# Included before project() by the top-level CMakeLists.txt and by
# core/CMakeLists.txt, so both configuration paths get the same number. Sets
#
#   UC2_CAM_VERSION   "MAJOR.MINOR.PATCH" (SemVer), e.g. "1.0.0"
#
# pyproject.toml reads the same file for the package metadata.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

set(_uc2_version_file "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
get_filename_component(_uc2_version_file "${_uc2_version_file}" ABSOLUTE)
if(NOT EXISTS "${_uc2_version_file}")
    message(FATAL_ERROR "uc2_cam_py: version file ${_uc2_version_file} not found")
endif()

file(STRINGS "${_uc2_version_file}" _uc2_version_lines LIMIT_COUNT 1)
string(STRIP "${_uc2_version_lines}" UC2_CAM_VERSION)
if(NOT UC2_CAM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "uc2_cam_py: ${_uc2_version_file} must hold MAJOR.MINOR.PATCH, "
                        "got '${UC2_CAM_VERSION}'")
endif()

# file(STRINGS) does not make the configure step depend on the file: without
# this, a changed VERSION would only be picked up by a manual re-configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_uc2_version_file}")

message(STATUS "uc2_cam_py version ${UC2_CAM_VERSION}")
