#include "SimCamera.h"

#include "SimPattern.h"
#include "hikLog.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace hik {

namespace {

const double kMaxFwhmPx  = 100.0;   ///< spot width (unbinned sensor pixels) on large sensors
const double kOffsetRatio = 0.1;    ///< constant background, as a fraction of the spot height
const double kMaxRate    = 200.0;   ///< "no frame-rate limit" runs at this rate
const double kMinExpMs   = 0.01;
const double kMaxExpMs   = 1000.0;
const double kMinRate    = 0.1;
const double kMaxGainDb  = 24.0;

} // namespace

const char* const SimCamera::kModel = "UC2-SIM";

bool SimCamera::pixelFormatByName(const std::string& name, int64_t& pfncCode)
{
    if (name == "Mono8")  { pfncCode = pfnc::Mono8;  return true; }
    if (name == "Mono16") { pfncCode = pfnc::Mono16; return true; }
    return false;
}

SimCamera::SimCamera(const Options& opt)
    : CameraBase("Simulated", kModel, opt.serial, opt.connectionType, 0)
    , m_opt(opt)
{
    if (opt.pixelFormat != pfnc::Mono8 && opt.pixelFormat != pfnc::Mono16)
    {
        throw std::invalid_argument("SimCamera: pixel format must be Mono8 or Mono16");
    }
    if (opt.sensorWidth < kMinWidth || opt.sensorHeight < kMinHeight)
    {
        throw std::invalid_argument("SimCamera: sensor too small");
    }
    m_frameRate  = clampValue(opt.frameRate, kMinRate, kMaxRate);
    m_roi.width  = opt.sensorWidth;
    m_roi.height = opt.sensorHeight;
    INFO("SimCamera: simulated " + std::to_string(opt.sensorWidth) + "x"
         + std::to_string(opt.sensorHeight) + " sensor, serial " + opt.serial);
}

SimCamera::~SimCamera()
{
    close();
}

sim::SpotPosition SimCamera::spotPositionFor(uint64_t sequence) const
{
    return sim::spotPosition(sequence, m_opt.sensorWidth, m_opt.sensorHeight);
}

// --- Ranges and values ------------------------------------------------------

ValueRange SimCamera::exposureRangeMs() { return ValueRange{ true, kMinExpMs, kMaxExpMs }; }
ValueRange SimCamera::frameRateRange()  { return ValueRange{ true, kMinRate, kMaxRate }; }
ValueRange SimCamera::gainRangeDb()     { return ValueRange{ true, 0.0, kMaxGainDb }; }

bool SimCamera::getExposureMs(double& ms)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    ms = m_exposureMs;
    return true;
}

bool SimCamera::getFrameRate(double& fps)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    fps = m_rateLimit ? m_frameRate : kMaxRate;
    return true;
}

bool SimCamera::getGainDb(double& db)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    db = m_gainDb;
    return true;
}

bool SimCamera::getBinning(uint32_t& factor)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    factor = m_binning;
    return true;
}

bool SimCamera::getRoi(SensorRoi& roi)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    roi = m_roi;
    return true;
}

bool SimCamera::getSensorSize(int64_t& width, int64_t& height)
{
    width  = m_opt.sensorWidth;
    height = m_opt.sensorHeight;
    return true;
}

bool SimCamera::getImageSize(int64_t& width, int64_t& height)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    width  = m_roi.width / m_binning;
    height = m_roi.height / m_binning;
    return true;
}

bool SimCamera::exposureAuto() const
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    return m_exposureAuto;
}

bool SimCamera::gainAuto() const
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    return m_gainAuto;
}

// --- Setters ----------------------------------------------------------------

bool SimCamera::setExposureMs(double ms)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_exposureAuto = false;
    m_exposureMs   = clampValue(ms, kMinExpMs, kMaxExpMs);
    return true;
}

bool SimCamera::setExposureAuto()
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_exposureAuto = true;
    return true;
}

bool SimCamera::setFrameRate(double fps)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_rateLimit = true;
    m_frameRate = clampValue(fps, kMinRate, kMaxRate);
    return true;
}

bool SimCamera::disableFrameRateLimit()
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_rateLimit = false;
    return true;
}

bool SimCamera::setGainDb(double db)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_gainAuto = false;
    m_gainDb   = clampValue(db, 0.0, kMaxGainDb);
    return true;
}

bool SimCamera::setGainAuto()
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    m_gainAuto = true;
    return true;
}

// --- Geometry ----------------------------------------------------------------

bool SimCamera::doSetBinning(uint32_t factor)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    const uint32_t b = nearestBinning(factor, { 1, 2, 4 });
    // Keep the ROI on the grid of the new binning (like real firmware does).
    SensorRoi d = toBinned(m_roi, b);
    const int64_t maxW = m_opt.sensorWidth / b;
    const int64_t maxH = m_opt.sensorHeight / b;
    d.width  = nearestOnGrid(d.width, kMinWidth, maxW, kWidthInc);
    d.height = nearestOnGrid(d.height, kMinHeight, maxH, kHeightInc);
    d.x      = nearestOnGrid(d.x, 0, maxW - d.width, kOffsetInc);
    d.y      = nearestOnGrid(d.y, 0, maxH - d.height, kOffsetInc);
    m_binning = b;
    m_roi     = toSensor(d, b);
    return true;
}

bool SimCamera::doSetRoi(const SensorRoi& roi)
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    const uint32_t b = m_binning;
    SensorRoi d = toBinned(roi, b);
    const int64_t maxW = m_opt.sensorWidth / b;
    const int64_t maxH = m_opt.sensorHeight / b;
    // Same order as GenICam firmware: size first, then the offsets whose
    // maximum depends on it.
    d.width  = nearestOnGrid(d.width, kMinWidth, maxW, kWidthInc);
    d.height = nearestOnGrid(d.height, kMinHeight, maxH, kHeightInc);
    d.x      = nearestOnGrid(d.x, 0, maxW - d.width, kOffsetInc);
    d.y      = nearestOnGrid(d.y, 0, maxH - d.height, kOffsetInc);
    m_roi = toSensor(d, b);
    return true;
}

bool SimCamera::doResetRoi()
{
    SensorRoi full;
    full.width  = m_opt.sensorWidth;
    full.height = m_opt.sensorHeight;
    return doSetRoi(full);
}

// --- Streaming ---------------------------------------------------------------

double SimCamera::periodSeconds() const
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    return 1.0 / (m_rateLimit ? m_frameRate : kMaxRate);
}

bool SimCamera::doStartStream()
{
    {
        std::lock_guard<std::mutex> lk(m_runMtx);
        m_stopRequested = false;
    }
    m_thread = std::thread(&SimCamera::run, this);
    return true;
}

void SimCamera::doStopStream()
{
    {
        std::lock_guard<std::mutex> lk(m_runMtx);
        m_stopRequested = true;
    }
    m_runCv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void SimCamera::doClose()
{
    // Nothing to release: the stream thread is already joined.
}

void SimCamera::run()
{
    SensorRoi roi;
    uint32_t  binning = 1;
    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        roi     = m_roi;
        binning = m_binning;
    }
    const PixelLayout lay    = pixelLayout(m_opt.pixelFormat);
    const uint64_t    bytes  = static_cast<uint64_t>(roi.width / binning) * (roi.height / binning)
                               * itemSize(lay.depth);
    // Spot plus offset reach 59 400 (Mono16) or 198 (Mono8). The noise there
    // (sigma 244 or 14) stays 25 or 4 sigma below the top of the range, so
    // clipping is negligible.
    const double      peak   = (lay.depth == PixelDepth::UInt16) ? 54000.0 : 180.0;
    const double      offset = kOffsetRatio * peak;
    // One third of a grid cell keeps the spot clear of the sensor edges, so its
    // centroid is exact; on the default 1920 x 1200 sensor that is 100 px.
    const double      fwhm   = std::min(kMaxFwhmPx,
                                        std::min(m_opt.sensorWidth / 5.0, m_opt.sensorHeight / 4.0) / 3.0);
    std::vector<uint8_t> frame(static_cast<size_t>(bytes));

    auto next = std::chrono::steady_clock::now();
    for (uint64_t k = 0;; ++k)
    {
        // Seeded with the frame number: a session always produces the same images.
        sim::renderNoisySpot(frame.data(), lay.depth, roi, binning, spotPositionFor(k), fwhm, peak,
                             offset, k);
        deliverFrame(frame.data(), bytes);

        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(periodSeconds()));
        std::unique_lock<std::mutex> lk(m_runMtx);
        if (m_runCv.wait_until(lk, next, [this] { return m_stopRequested; }))
        {
            return;
        }
    }
}

} // namespace hik
