#ifndef UC2HIK_HIKCAMERA_H
#define UC2HIK_HIKCAMERA_H

#include <memory>

#include "CameraBase.h"
#include "MvsCamera.h"

namespace hik {

/// HikRobot (MVS SDK) implementation of CameraBase.
///
/// Opens the selected device in free-run mode (TriggerMode = Off) and makes
/// sure its pixel format can be stored (packed formats are switched to their
/// unpacked equivalent). Frames arrive through the SDK image callback
/// (bAutoFree = true): the callback copies them into the ring and the SDK gets
/// its buffer back immediately.
///
/// Firmware units: ExposureTime in us (converted to ms here), Gain in dB,
/// AcquisitionFrameRate in fps. ROI nodes count BINNED pixels; the interface
/// works in unbinned sensor pixels and converts.
class HikCamera : public CameraBase
{
public:
    /// Open a camera.
    /// @param connectionType "USB3", "GigE" or "" (any)
    /// @param serial         serial number to open, or "" for none
    /// @param index          index into the devices of @p connectionType,
    ///                       or < 0 for none. Without serial and index the
    ///                       first camera is opened.
    /// @throws std::invalid_argument for an unknown connection type,
    ///         std::runtime_error if no matching camera can be opened.
    HikCamera(const std::string& connectionType, const std::string& serial, int index);
    ~HikCamera() override;

    bool canSetExposure() override;
    bool canSetGain() override;
    bool canSetFrameRate() override;
    bool canSetBinning() override;
    bool canSetTrigger() override;
    bool canSetRoi() override;

    ValueRange exposureRangeMs() override;
    ValueRange frameRateRange() override;
    ValueRange gainRangeDb() override;
    std::vector<uint32_t> binningValues() override;

    bool getExposureMs(double& ms) override;
    bool getFrameRate(double& fps) override;
    bool getGainDb(double& db) override;
    bool getBinning(uint32_t& factor) override;
    bool getRoi(SensorRoi& roi) override;
    bool getSensorSize(int64_t& width, int64_t& height) override;
    bool getImageSize(int64_t& width, int64_t& height) override;
    int64_t pixelFormat() override;

    bool setExposureMs(double ms) override;
    bool setExposureAuto() override;
    bool setFrameRate(double fps) override;
    bool disableFrameRateLimit() override;
    bool setGainDb(double db) override;
    bool setGainAuto() override;

protected:
    bool doSetBinning(uint32_t factor) override;
    bool doSetRoi(const SensorRoi& roi) override;
    bool doResetRoi() override;
    bool doStartStream() override;
    void doStopStream() override;
    void doClose() override;

private:
    struct Opened
    {
        std::unique_ptr<MvsCamera> camera;
        int                        index = -1;
    };
    static Opened openDevice(const std::string& connectionType, const std::string& serial, int index);
    explicit HikCamera(Opened&& opened);

    static void __stdcall onImage(MV_FRAME_OUT* frame, void* user, bool autoFree);

    void ensureStorablePixelFormat();
    bool writeBinning(const char* key, uint32_t factor);
    bool readBinning(const char* key, uint32_t& factor);

    std::unique_ptr<MvsCamera> m_cam;
    bool                       m_canBinning = false;   ///< evaluated while stopped (nodes are RO while grabbing)
    bool                       m_canRoi     = false;
};

} // namespace hik

#endif // UC2HIK_HIKCAMERA_H
