#ifndef UC2HIK_VERSION_H
#define UC2HIK_VERSION_H

/// @file UC2Version.h
/// @brief Version of uc2_cam_py, as compiled into this binary.
///
/// The number comes from the file VERSION in the repository root (CMake:
/// core/cmake/UC2Version.cmake). Only src/Version.cpp sees it, so a new
/// version recompiles that one file.

namespace hik {

/// @return "MAJOR.MINOR.PATCH" (SemVer), e.g. "1.0.0".
const char* version();

} // namespace hik

#endif // UC2HIK_VERSION_H
