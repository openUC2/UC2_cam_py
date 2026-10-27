#ifndef UC2HIK_DEVICESUMMARY_H
#define UC2HIK_DEVICESUMMARY_H

#include <string>

namespace hik {

/// Short, human-readable description of one enumerated device.
///
/// Free of SDK types, so it also exists in a build without the MVS SDK (the
/// Python `DeviceInfo`); MvsCamera.h extends it by the SDK's own descriptor.
struct DeviceSummary
{
    std::string transport;   ///< "GigE", "USB3", "GenTL-GigE", ...
    std::string modelName;   ///< camera model
    std::string serial;      ///< serial number
    std::string userName;    ///< user-defined name (may be empty)
    std::string ip;          ///< current IP (GigE only, else empty)
};

} // namespace hik

#endif // UC2HIK_DEVICESUMMARY_H
