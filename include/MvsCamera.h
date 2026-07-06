#ifndef HIKCAMTEST_MVSCAMERA_H
#define HIKCAMTEST_MVSCAMERA_H

#include <string>
#include <vector>

#include "MvCameraControl.h"

namespace hik {

/// Short, human-readable description of one enumerated device.
struct DeviceInfo
{
    std::string transport;   ///< "GigE", "USB3", "GenTL-GigE", ...
    std::string modelName;   ///< camera model
    std::string serial;      ///< serial number
    std::string userName;    ///< user-defined name (may be empty)
    std::string ip;          ///< current IP (GigE only, else empty)
};

/// Format used when saving a frame to disk.
enum class SaveFormat
{
    Bmp,
    Jpeg,
    Png,
    Tiff
};

/// Thin RAII wrapper around the HikRobot / MVS "MvCameraControl" C API for a
/// single area-scan camera: enumerate -> open -> grab -> display/save -> close.
///
/// The class owns exactly one device handle. Copying is disabled so the handle
/// lifetime is unambiguous.
class MvsCamera
{
public:
    MvsCamera();
    ~MvsCamera();

    MvsCamera(const MvsCamera&) = delete;
    MvsCamera& operator=(const MvsCamera&) = delete;

    /// Initialise / finalise the SDK. Call initSdk() once at program start and
    /// finalizeSdk() once at program end. Returns true on success.
    static bool initSdk();
    static void finalizeSdk();

    /// Enumerate all reachable GigE and USB3 devices. The result is cached
    /// internally so open(index) can be called afterwards. Returns the number
    /// of devices found, or a negative value on error.
    int enumerateDevices();

    /// Human-readable copy of the last enumeration result.
    const std::vector<DeviceInfo>& devices() const { return m_devices; }

    /// Open the device at the given index (into the last enumeration), create a
    /// handle, tune the GigE packet size and switch the camera to continuous
    /// (free-run) acquisition. Returns true on success.
    bool open(unsigned int index);

    /// Stop grabbing (if needed), close the device and destroy the handle.
    void close();

    bool startGrabbing();
    bool stopGrabbing();

    /// Block up to timeoutMs for the next frame. On success the caller owns the
    /// buffer until freeFrame() is called. Returns true on success.
    bool getFrame(MV_FRAME_OUT& frame, unsigned int timeoutMs);

    /// Release a buffer previously obtained from getFrame().
    void freeFrame(MV_FRAME_OUT& frame);

    /// Let the SDK render a grabbed frame into a native window handle
    /// (an X11 Window passed as void*). No manual pixel conversion needed.
    bool displayFrame(void* windowHandle, const MV_FRAME_OUT& frame);

    /// Save a grabbed frame to disk. The SDK performs any needed pixel-format
    /// conversion (e.g. Bayer -> RGB). Returns true on success.
    bool saveFrame(const MV_FRAME_OUT& frame, const std::string& path, SaveFormat format);

    bool isOpen() const { return m_handle != nullptr; }
    bool isGrabbing() const { return m_grabbing; }
    void* handle() const { return m_handle; }

private:
    void*                   m_handle;      ///< SDK device handle
    MV_CC_DEVICE_INFO_LIST  m_deviceList;  ///< raw list from last enumeration
    std::vector<DeviceInfo> m_devices;     ///< readable view of m_deviceList
    bool                    m_grabbing;
};

} // namespace hik

#endif // HIKCAMTEST_MVSCAMERA_H
