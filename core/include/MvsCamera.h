#ifndef UC2HIK_MVSCAMERA_H
#define UC2HIK_MVSCAMERA_H

#include <cstdint>
#include <memory>
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
    MV_CC_DEVICE_INFO raw{};     // POD, safe to copy by value
};

/// Format used when saving a frame to disk.
enum class SaveFormat
{
    Bmp,
    Jpeg,
    Png,
    Tiff
};

/// Auto-regulation mode for exposure and gain. Values match the MVS SDK enums
/// (MV_EXPOSURE_AUTO_MODE_* / MV_CAM_GAIN_MODE_*).
enum class AutoMode
{
    Off        = 0,   ///< firmware regulation disabled; the manual value is used
    Once       = 1,   ///< regulate once, then hold
    Continuous = 2    ///< regulate continuously
};

/// Region of interest (in pixels) on the sensor.
struct Roi
{
    int64_t width   = 0;   ///< ROI width
    int64_t height  = 0;   ///< ROI height
    int64_t offsetX = 0;   ///< horizontal offset from the left sensor edge
    int64_t offsetY = 0;   ///< vertical offset from the top sensor edge
};

/// Clamp @p value to [@p min, @p max] and round it down to a multiple of @p inc.
///
/// Used to make a requested ROI dimension valid for the camera (which only
/// accepts values on an increment grid within a min/max range).
///
/// @param value requested value.
/// @param min   minimum accepted value.
/// @param max   maximum accepted value.
/// @param inc   increment/step the value must be a multiple of (>= 1).
/// @return the largest valid value <= the clamped request.
int64_t alignRoiValue(int64_t value, int64_t min, int64_t max, int64_t inc);

// Process-wide SDK lifetime, reference counted. MV_CC_Finalize() runs when the
// last holder is released, so cameras can be created and destroyed freely.
class MvsSdk
{
public:
    static std::shared_ptr<MvsSdk> acquire();  // MV_CC_Initialize() on first call
    ~MvsSdk();

    MvsSdk(const MvsSdk&)            = delete;
    MvsSdk& operator=(const MvsSdk&) = delete;

private:
    MvsSdk() = default;
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

    // move constructors are needed to allow std::vector<MvsCamera> in CaptureApp.
    // move constructor and move assignment operator are noexcept to allow std::vector<MvsCamera> to be moved without throwing exceptions.
    MvsCamera(MvsCamera&&) noexcept;
    MvsCamera& operator=(MvsCamera&&) noexcept;

    MvsCamera(const MvsCamera&) = delete;
    MvsCamera& operator=(const MvsCamera&) = delete;

    /// Initialise / finalise the SDK explicitly. Both are thin wrappers around
    /// the reference counted MvsSdk: initSdk() takes a reference that is held
    /// until finalizeSdk() drops it, so init/finalize pairs may be repeated and
    /// may be nested with live MvsCamera instances (which hold their own
    /// reference). Returns true on success.
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

    // --- Camera parameters (require an open device) --------------------------
    // Exposure time is in microseconds, gain in dB, frame rate in frames/second.

    /// Set the exposure time. Switches ExposureAuto to Off so the value sticks.
    /// @param microseconds exposure time in microseconds.
    /// @return true on success.
    bool setExposureTime(double microseconds);
    /// Read the current exposure time.
    /// @param microseconds [out] exposure time in microseconds.
    /// @return true on success.
    bool getExposureTime(double& microseconds);
    /// Read the exposure-time limits.
    /// @param minUs [out] minimum exposure time in microseconds.
    /// @param maxUs [out] maximum exposure time in microseconds.
    /// @return true on success.
    bool getExposureTimeRange(double& minUs, double& maxUs);

    /// Set the analog gain. Switches GainAuto to Off so the value sticks.
    /// @param db gain in decibels.
    /// @return true on success.
    bool setGain(double db);
    /// Read the current gain.
    /// @param db [out] gain in decibels.
    /// @return true on success.
    bool getGain(double& db);
    /// Read the gain limits.
    /// @param minDb [out] minimum gain in decibels.
    /// @param maxDb [out] maximum gain in decibels.
    /// @return true on success.
    bool getGainRange(double& minDb, double& maxDb);

    /// Enable manual frame-rate control and set the acquisition frame rate.
    /// @param fps requested frame rate in frames per second.
    /// @return true on success.
    bool setFrameRate(double fps);
    /// Read the configured acquisition frame rate.
    /// @param fps [out] frame rate in frames per second.
    /// @return true on success.
    bool getFrameRate(double& fps);
    /// Read the resulting (actual) frame rate the camera can achieve.
    /// @param fps [out] resulting frame rate in frames per second.
    /// @return true on success (false if the node is unavailable).
    bool getResultingFrameRate(double& fps);
    /// Read the acquisition frame-rate limits.
    /// @param minFps [out] minimum frame rate.
    /// @param maxFps [out] maximum frame rate.
    /// @return true on success.
    bool getFrameRateRange(double& minFps, double& maxFps);

    /// Set the auto-exposure mode.
    /// @param mode Off, Once or Continuous.
    /// @return true on success.
    bool setExposureAuto(AutoMode mode);
    /// Read the auto-exposure mode.
    /// @param mode [out] current mode.
    /// @return true on success.
    bool getExposureAuto(AutoMode& mode);
    /// Set the auto-gain mode.
    /// @param mode Off, Once or Continuous.
    /// @return true on success.
    bool setGainAuto(AutoMode mode);
    /// Read the auto-gain mode.
    /// @param mode [out] current mode.
    /// @return true on success.
    bool getGainAuto(AutoMode& mode);

    /// Set the region of interest. The camera only accepts this while NOT
    /// grabbing; the requested values are clamped and aligned to the camera's
    /// increment grid, and written in the order offsets->size->offsets.
    /// @param roi requested width/height/offsetX/offsetY in pixels.
    /// @return true on success.
    bool setRoi(const Roi& roi);
    /// Read the current region of interest.
    /// @param out [out] current width/height/offsetX/offsetY.
    /// @return true on success.
    bool getRoi(Roi& out);
    /// Read the full sensor size (WidthMax/HeightMax).
    /// @param widthMax  [out] maximum ROI width.
    /// @param heightMax [out] maximum ROI height.
    /// @return true on success.
    bool getSensorSize(int64_t& widthMax, int64_t& heightMax);

private:
    // Generic GenICam node accessors (any out pointer may be null).
    bool setFloatNode(const char* key, float value);
    bool getFloatNode(const char* key, float* cur, float* min, float* max);
    bool setIntNode(const char* key, int64_t value);
    bool getIntNode(const char* key, int64_t* cur, int64_t* min, int64_t* max, int64_t* inc);
    bool setEnumNode(const char* key, unsigned int value);
    bool getEnumNode(const char* key, unsigned int* cur);
    bool setBoolNode(const char* key, bool value);

    void*                   m_handle = nullptr;  ///< SDK device handle
    std::shared_ptr<MvsSdk> m_sdk;       // keeps the SDK alive while this camera lives
    bool                    m_grabbing = false;
    DeviceInfo              m_info;      ///< the currently open device (empty while closed)
    std::vector<DeviceInfo> m_devices;   ///< result of the last enumeration
};

} // namespace hik

#endif // UC2HIK_MVSCAMERA_H
