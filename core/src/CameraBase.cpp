#include "CameraBase.h"

#include "hikLog.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

namespace hik {

// ---------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------

namespace {

const int64_t kMono10p = 0x010A0046;   // PFNC "p" packing (LSB-first)
const int64_t kMono12p = 0x010C0047;

PixelLayout layout(uint32_t channels, PixelDepth depth, const char* name)
{
    PixelLayout l;
    l.supported = true;
    l.channels  = channels;
    l.depth     = depth;
    l.name      = name;
    return l;
}

PixelLayout unsupported(const char* name)
{
    PixelLayout l;
    l.name = name;
    return l;
}

} // namespace

PixelLayout pixelLayout(int64_t pf)
{
    switch (pf)
    {
    case pfnc::Mono8:      return layout(1, PixelDepth::UInt8, "Mono8");
    case pfnc::Mono10:     return layout(1, PixelDepth::UInt16, "Mono10");
    case pfnc::Mono12:     return layout(1, PixelDepth::UInt16, "Mono12");
    case pfnc::Mono16:     return layout(1, PixelDepth::UInt16, "Mono16");
    case pfnc::BayerGR8:   return layout(1, PixelDepth::UInt8, "BayerGR8");
    case pfnc::BayerRG8:   return layout(1, PixelDepth::UInt8, "BayerRG8");
    case pfnc::BayerGB8:   return layout(1, PixelDepth::UInt8, "BayerGB8");
    case pfnc::BayerBG8:   return layout(1, PixelDepth::UInt8, "BayerBG8");
    case pfnc::BayerGR10:        return layout(1, PixelDepth::UInt16, "BayerGR10");
    case pfnc::BayerGR10 + 1:    return layout(1, PixelDepth::UInt16, "BayerRG10");
    case pfnc::BayerGR10 + 2:    return layout(1, PixelDepth::UInt16, "BayerGB10");
    case pfnc::BayerBG10:        return layout(1, PixelDepth::UInt16, "BayerBG10");
    case pfnc::BayerGR12:        return layout(1, PixelDepth::UInt16, "BayerGR12");
    case pfnc::BayerGR12 + 1:    return layout(1, PixelDepth::UInt16, "BayerRG12");
    case pfnc::BayerGR12 + 2:    return layout(1, PixelDepth::UInt16, "BayerGB12");
    case pfnc::BayerBG12:        return layout(1, PixelDepth::UInt16, "BayerBG12");
    case pfnc::BayerGR16:        return layout(1, PixelDepth::UInt16, "BayerGR16");
    case pfnc::BayerGR16 + 1:    return layout(1, PixelDepth::UInt16, "BayerRG16");
    case pfnc::BayerGR16 + 2:    return layout(1, PixelDepth::UInt16, "BayerGB16");
    case pfnc::BayerBG16:        return layout(1, PixelDepth::UInt16, "BayerBG16");
    case pfnc::RGB8:       return layout(3, PixelDepth::UInt8, "RGB8");
    case pfnc::BGR8:       return layout(3, PixelDepth::UInt8, "BGR8");
    case pfnc::Mono10Packed:  return unsupported("Mono10Packed");
    case pfnc::Mono12Packed:  return unsupported("Mono12Packed");
    case kMono10p:            return unsupported("Mono10p");
    case kMono12p:            return unsupported("Mono12p");
    case pfnc::YUV422_8:      return unsupported("YUV422_8");
    case pfnc::YUV422_8_UYVY: return unsupported("YUV422_8_UYVY");
    default:
        if (pf >= pfnc::BayerGR10Packed && pf <= pfnc::BayerBG12Packed)
        {
            return unsupported("Bayer*Packed");
        }
        return unsupported("unknown");
    }
}

std::vector<int64_t> unpackedAlternatives(int64_t pf)
{
    if (pf == pfnc::Mono10Packed || pf == kMono10p) { return { pfnc::Mono10 }; }
    if (pf == pfnc::Mono12Packed || pf == kMono12p) { return { pfnc::Mono12 }; }
    if (pf >= pfnc::BayerGR10Packed && pf <= pfnc::BayerGR10Packed + 3)
    {
        // BayerGR/RG/GB/BG10_Packed (0x26..0x29) -> BayerXX10 (0x0C..0x0F)
        return { pfnc::BayerGR10 + (pf - pfnc::BayerGR10Packed) };
    }
    if (pf >= pfnc::BayerGR10Packed + 4 && pf <= pfnc::BayerBG12Packed)
    {
        // BayerGR/RG/GB/BG12_Packed (0x2A..0x2D) -> BayerXX12 (0x10..0x13)
        return { pfnc::BayerGR12 + (pf - (pfnc::BayerGR10Packed + 4)) };
    }
    if (pf == pfnc::YUV422_8 || pf == pfnc::YUV422_8_UYVY)
    {
        return { pfnc::RGB8, pfnc::BGR8 };
    }
    return {};
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

int64_t nearestOnGrid(int64_t value, int64_t min, int64_t max, int64_t inc)
{
    if (inc < 1)
    {
        inc = 1;
    }
    if (max < min)
    {
        max = min;
    }
    // Largest grid point that does not exceed max.
    const int64_t top = min + ((max - min) / inc) * inc;
    if (value <= min)
    {
        return min;
    }
    if (value >= top)
    {
        return top;
    }
    const int64_t k = (value - min + inc / 2) / inc;   // nearest, ties up
    const int64_t snapped = min + k * inc;
    return (snapped > top) ? top : snapped;
}

uint32_t nearestBinning(uint32_t requested, const std::vector<uint32_t>& supported)
{
    if (supported.empty())
    {
        return 1;
    }
    uint32_t best     = supported.front();
    uint64_t bestDist = UINT64_MAX;
    for (uint32_t s : supported)
    {
        const uint64_t d = (s > requested) ? (s - requested) : (requested - s);
        if (d < bestDist || (d == bestDist && s < best))
        {
            best     = s;
            bestDist = d;
        }
    }
    return best;
}

double clampValue(double value, double lo, double hi)
{
    if (value < lo) { return lo; }
    if (value > hi) { return hi; }
    return value;
}

SensorRoi toBinned(const SensorRoi& s, uint32_t binning)
{
    const int64_t b = (binning < 1) ? 1 : binning;
    SensorRoi r;
    r.x      = s.x / b;
    r.y      = s.y / b;
    r.width  = s.width / b;
    r.height = s.height / b;
    return r;
}

SensorRoi toSensor(const SensorRoi& d, uint32_t binning)
{
    const int64_t b = (binning < 1) ? 1 : binning;
    SensorRoi r;
    r.x      = d.x * b;
    r.y      = d.y * b;
    r.width  = d.width * b;
    r.height = d.height * b;
    return r;
}

// ---------------------------------------------------------------------------
// CameraBase
// ---------------------------------------------------------------------------

CameraBase::CameraBase(std::string vendor, std::string model, std::string serial,
                       std::string connectionType, int index)
    : m_vendor(std::move(vendor))
    , m_model(std::move(model))
    , m_serial(std::move(serial))
    , m_connectionType(std::move(connectionType))
    , m_index(index)
    , m_active(nullptr)
    , m_inFlight(0)
    , m_running(false)
    , m_rejected(0)
    , m_sizeWarned(false)
{
}

CameraBase::~CameraBase() = default;

void CameraBase::setBufferConfig(const BufferConfig& cfg)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg = cfg;
}

CameraBase::BufferConfig CameraBase::bufferConfig() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_cfg;
}

bool CameraBase::start()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return startLocked();
}

void CameraBase::stop()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    stopLocked();
}

void CameraBase::close()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    stopLocked();
    if (!m_closed)
    {
        doClose();
        m_closed = true;
        DBG("CameraBase: closed " + m_vendor + " " + m_serial);
    }
}

bool CameraBase::startLocked()
{
    if (m_closed)
    {
        ERR("CameraBase::start: the camera has been closed");
        return false;
    }
    if (m_running.load())
    {
        return true;
    }

    int64_t width = 0, height = 0;
    if (!getImageSize(width, height) || width <= 0 || height <= 0)
    {
        ERR("CameraBase::start: cannot read the current image size");
        return false;
    }
    const int64_t     pf  = pixelFormat();
    const PixelLayout lay = pixelLayout(pf);
    if (!lay.supported)
    {
        std::ostringstream os;
        os << "CameraBase::start: pixel format " << lay.name << " (0x" << std::hex << pf
           << ") cannot be stored; use Mono8/10/12/16, Bayer*8/10/12/16 or RGB8/BGR8";
        ERR(os.str());
        return false;
    }

    RingBuffer::Config rc;
    rc.storagePath = m_cfg.storagePath;
    rc.slotCount   = m_cfg.slotCount;
    rc.comment     = m_cfg.comment;
    rc.policy      = FullPolicy::Overwrite;

    auto ring = std::make_shared<RingBuffer>(rc);
    ring->setImageSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    ring->setChannels(lay.channels);
    ring->setDepth(lay.depth);
    ring->setCameraId(m_model + "-" + m_serial);
    if (!ring->configure())
    {
        return false;   // configure() logged why
    }
    if (!ring->startRecording())
    {
        ERR("CameraBase::start: cannot open the recording session in '" + m_cfg.storagePath + "'");
        return false;
    }

    m_rejected.store(0);
    m_sizeWarned.store(false);
    m_active.store(ring.get());
    if (!doStartStream())
    {
        m_active.store(nullptr);
        while (m_inFlight.load() != 0)
        {
            std::this_thread::yield();
        }
        ring->stopRecording();
        ERR("CameraBase::start: the device did not start streaming");
        return false;
    }

    m_ring = ring;
    m_running.store(true);
    INFO("Acquisition started: " + std::to_string(width) + "x" + std::to_string(height) + " "
         + lay.name + ", " + std::to_string(m_cfg.slotCount) + " slots"
         + (m_cfg.storagePath.empty() ? std::string(", not stored")
                                      : ", recording to " + ring->streamFilePath()));
    return true;
}

void CameraBase::stopLocked()
{
    if (!m_running.load())
    {
        return;
    }
    // No new frame may enter the ring from here on; wait for the ones that
    // are already being copied before the session is closed under them.
    m_active.store(nullptr);
    doStopStream();
    while (m_inFlight.load() != 0)
    {
        std::this_thread::yield();
    }
    if (m_ring)
    {
        m_ring->stopRecording();
        INFO("Acquisition stopped after " + std::to_string(m_ring->framesWritten()) + " frames"
             + (m_rejected.load() != 0
                    ? " (" + std::to_string(m_rejected.load()) + " rejected)"
                    : std::string()));
    }
    m_running.store(false);
}

void CameraBase::deliverFrame(const void* data, uint64_t nbytes)
{
    const int64_t tsBefore = monotonicNs();
    m_inFlight.fetch_add(1);
    RingBuffer* rb = m_active.load();
    if (rb != nullptr && data != nullptr)
    {
        if (nbytes != rb->frameBytes())
        {
            m_rejected.fetch_add(1);
            if (!m_sizeWarned.exchange(true))
            {
                WARN("CameraBase: frame of " + std::to_string(nbytes) + " bytes does not match the "
                     + std::to_string(rb->frameBytes()) + " byte slot - dropping such frames");
            }
        }
        else
        {
            RingBuffer::WriteHandle w;
            if (rb->beginWrite(w, tsBefore))
            {
                std::memcpy(w.data(), data, static_cast<size_t>(nbytes));   // the one copy
                rb->commitWrite(w, nbytes);
            }
            else
            {
                m_rejected.fetch_add(1);
            }
        }
    }
    m_inFlight.fetch_sub(1);
}

template <typename F>
bool CameraBase::changeGeometry(F change)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_lastRestarted = false;
    if (m_closed)
    {
        ERR("CameraBase: the camera has been closed");
        return false;
    }
    const bool wasRunning = m_running.load();
    if (wasRunning)
    {
        stopLocked();
    }
    const bool ok = change();
    if (wasRunning)
    {
        const bool restarted = startLocked();
        m_lastRestarted = restarted;
        if (restarted)
        {
            INFO("Acquisition restarted for the new image geometry (new session)");
        }
        else
        {
            ERR("CameraBase: acquisition could not be restarted after the geometry change");
        }
        return ok && restarted;
    }
    return ok;
}

bool CameraBase::setBinning(uint32_t factor)
{
    return changeGeometry([this, factor] { return doSetBinning(factor); });
}

bool CameraBase::setRoi(const SensorRoi& roi)
{
    return changeGeometry([this, &roi] { return doSetRoi(roi); });
}

bool CameraBase::resetRoi()
{
    return changeGeometry([this] { return doResetRoi(); });
}

std::shared_ptr<RingBuffer> CameraBase::ring() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_ring;
}

bool CameraBase::readLatest(Image& out)
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r)
    {
        return false;
    }
    if (!r->readLatest(out.handle))
    {
        return false;
    }
    out.ring   = r;
    out.header = r->sessionHeader();
    return true;
}

bool CameraBase::readSlot(uint32_t slot, Image& out)
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r || slot >= r->slotCount())
    {
        return false;
    }
    if (!r->readSlot(slot, out.handle))
    {
        return false;
    }
    out.ring   = r;
    out.header = r->sessionHeader();
    return true;
}

bool CameraBase::sessionHeader(UC2_SessionHeader& out) const
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r)
    {
        return false;
    }
    out = r->sessionHeader();
    return true;
}

std::string CameraBase::recordingFile() const
{
    std::shared_ptr<RingBuffer> r = ring();
    return r ? r->streamFilePath() : std::string();
}

} // namespace hik
