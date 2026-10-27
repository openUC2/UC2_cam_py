#ifndef UC2HIK_SIMCAMERA_H
#define UC2HIK_SIMCAMERA_H

#include <condition_variable>
#include <mutex>
#include <thread>

#include "CameraBase.h"
#include "SimPattern.h"

namespace hik {

/// Simulated camera ("Simulated" vendor) for tests and development without
/// hardware. It produces the walking Gaussian spot of SimPattern.h through the
/// very same CameraBase -> RingBuffer path a real camera uses, on its own
/// delivery thread at the configured frame rate.
///
/// Simulated capabilities: exposure, gain, frame rate, binning {1, 2, 4} and
/// ROI are settable, the trigger is not. Exposure and gain are bookkeeping
/// only - they do not change the image, so the spot position stays exactly
/// predictable (spotPositionFor()).
class SimCamera : public CameraBase
{
public:
    struct Options
    {
        std::string serial         = "SIM-0001";
        std::string connectionType = "USB3";            ///< mirrored, "USB3" or "GigE"
        int64_t     pixelFormat    = pfnc::Mono16;      ///< Mono8 or Mono16
        uint32_t    sensorWidth    = 1920;
        uint32_t    sensorHeight   = 1200;
        double      frameRate      = 10.0;              ///< initial frame rate
    };

    static const char* const kModel;   ///< "UC2-SIM"

    /// PFNC code of the pixel format called @p name ("Mono8" or "Mono16").
    /// @return false for any other name.
    static bool pixelFormatByName(const std::string& name, int64_t& pfncCode);

    explicit SimCamera(const Options& opt);
    ~SimCamera() override;

    /// Spot centre (unbinned sensor pixels) of the frame with ring sequence
    /// @p sequence of the current session.
    sim::SpotPosition spotPositionFor(uint64_t sequence) const;

    bool canSetExposure() override  { return true; }
    bool canSetGain() override      { return true; }
    bool canSetFrameRate() override { return true; }
    bool canSetBinning() override   { return true; }
    bool canSetTrigger() override   { return false; }
    bool canSetRoi() override       { return true; }

    ValueRange exposureRangeMs() override;
    ValueRange frameRateRange() override;
    ValueRange gainRangeDb() override;
    std::vector<uint32_t> binningValues() override { return { 1, 2, 4 }; }

    bool getExposureMs(double& ms) override;
    bool getFrameRate(double& fps) override;
    bool getGainDb(double& db) override;
    bool getBinning(uint32_t& factor) override;
    bool getRoi(SensorRoi& roi) override;
    bool getSensorSize(int64_t& width, int64_t& height) override;
    bool getImageSize(int64_t& width, int64_t& height) override;
    int64_t pixelFormat() override { return m_opt.pixelFormat; }

    bool setExposureMs(double ms) override;
    bool setExposureAuto() override;
    bool setFrameRate(double fps) override;
    bool disableFrameRateLimit() override;
    bool setGainDb(double db) override;
    bool setGainAuto() override;

    /// @return true while exposure / gain are in (simulated) auto mode.
    bool exposureAuto() const;
    bool gainAuto() const;

protected:
    bool doSetBinning(uint32_t factor) override;
    bool doSetRoi(const SensorRoi& roi) override;
    bool doResetRoi() override;
    bool doStartStream() override;
    void doStopStream() override;
    void doClose() override;

private:
    // ROI grid of the simulated firmware, in binned pixels.
    static const int64_t kWidthInc  = 8;
    static const int64_t kHeightInc = 2;
    static const int64_t kOffsetInc = 2;
    static const int64_t kMinWidth  = 16;
    static const int64_t kMinHeight = 8;

    void run();
    double periodSeconds() const;

    Options                 m_opt;
    mutable std::mutex      m_stateMtx;   ///< guards the settings below
    double                  m_exposureMs  = 10.0;
    double                  m_gainDb      = 0.0;
    double                  m_frameRate   = 10.0;
    bool                    m_rateLimit   = true;
    bool                    m_exposureAuto = false;
    bool                    m_gainAuto    = false;
    uint32_t                m_binning     = 1;
    SensorRoi               m_roi;        ///< unbinned sensor pixels

    // Delivery thread.
    std::thread             m_thread;
    std::mutex              m_runMtx;
    std::condition_variable m_runCv;
    bool                    m_stopRequested = false;
};

} // namespace hik

#endif // UC2HIK_SIMCAMERA_H
