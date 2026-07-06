#ifndef HIKCAMTEST_MVSERROR_H
#define HIKCAMTEST_MVSERROR_H

#include <string>

namespace hik {

/// Return a human-readable description for an MVS SDK return code.
/// Falls back to the raw hex value for codes that are not mapped.
std::string mvsErrorString(int code);

/// Convenience: format a code as e.g. "MV_E_NORESPONSE (0x8000001a)".
std::string mvsErrorLabel(int code);

} // namespace hik

#endif // HIKCAMTEST_MVSERROR_H
