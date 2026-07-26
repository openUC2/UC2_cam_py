#include "FrameGrabber.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "MvsError.h"

namespace hik {

void pixelInfo(int pixelType, int& channels, int& itemsize, bool& simple)
{
    switch (pixelType)
    {
    case PixelType_Gvsp_Mono8:
        channels = 1; itemsize = 1; simple = true; break;
    case PixelType_Gvsp_RGB8_Packed:
    case PixelType_Gvsp_BGR8_Packed:
        channels = 3; itemsize = 1; simple = true; break;
    default:
        // Packed/Bayer/16-bit formats: leave interpretation to the caller and
        // expose the raw bytes instead of guessing a shape.
        channels = 0; itemsize = 1; simple = false; break;
    }
}

namespace {

/// Build a FrameView from a raw SDK frame struct.
FrameView makeView(const MV_FRAME_OUT& f)
{
    FrameView v;
    v.valid         = (f.pBufAddr != nullptr);
    v.data          = f.pBufAddr;
    v.nbytes        = (f.stFrameInfo.nFrameLenEx != 0)
                          ? f.stFrameInfo.nFrameLenEx
                          : f.stFrameInfo.nFrameLen;
    v.width         = f.stFrameInfo.nExtendWidth;
    v.height        = f.stFrameInfo.nExtendHeight;
    v.pixelType     = static_cast<int>(f.stFrameInfo.enPixelType);
    v.frameNum      = f.stFrameInfo.nFrameNum;
    v.hostTimestamp = f.stFrameInfo.nHostTimeStamp;
    v.lostPacket    = f.stFrameInfo.nLostPacket;
    return v;
}

} // namespace

FrameGrabber::FrameGrabber()
    : m_hasPending(false)
    , m_hasHeld(false)
    , m_running(false)
    , m_received(0)
    , m_dropped(0)
    , m_nodeNum(5)
    , m_payloadSize(0)
    , m_lastWidth(0)
    , m_lastHeight(0)
    , m_lastPixelType(0)
{
    std::memset(&m_pending, 0, sizeof(m_pending));
    std::memset(&m_held, 0, sizeof(m_held));
}

FrameGrabber::~FrameGrabber()
{
    close();
}

bool FrameGrabber::open(unsigned int index)
{
    if (m_camera.isOpen())
    {
        return true;
    }
    if (m_camera.enumerateDevices() <= 0)
    {
        return false;
    }
    if (!m_camera.open(index))
    {
        return false;
    }

    // Best-effort read of the payload size; not fatal if it fails.
    MVCC_INTVALUE payload;
    std::memset(&payload, 0, sizeof(payload));
    if (MV_CC_GetIntValue(m_camera.handle(), "PayloadSize", &payload) == MV_OK)
    {
        m_payloadSize = payload.nCurValue;
    }
    return true;
}

void FrameGrabber::setNodeNum(unsigned int n)
{
    // Keep at least three nodes: one held by the consumer, one pending and one
    // being filled by the camera.
    m_nodeNum = (n < 3u) ? 3u : n;
}

bool FrameGrabber::start()
{
    if (!m_camera.isOpen())
    {
        std::fprintf(stderr, "FrameGrabber::start: device is not open\n");
        return false;
    }
    if (m_running.load())
    {
        return true;
    }

    void* handle = m_camera.handle();

    int ret = MV_CC_SetImageNodeNum(handle, m_nodeNum);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetImageNodeNum failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }

    ret = MV_CC_SetGrabStrategy(handle, MV_GrabStrategy_OneByOne);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetGrabStrategy failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }

    // bAutoFree = false: we keep the node until we return it with FreeImageBuffer.
    ret = MV_CC_RegisterImageCallBackEx2(handle, &FrameGrabber::onFrame, this, false);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_RegisterImageCallBackEx2 failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }

    if (!m_camera.startGrabbing())
    {
        MV_CC_RegisterImageCallBackEx2(handle, nullptr, nullptr, false);
        return false;
    }

    m_running.store(true);
    return true;
}

void FrameGrabber::onFrame(MV_FRAME_OUT* frame, void* user, bool /*autoFree*/)
{
    if (user != nullptr && frame != nullptr)
    {
        static_cast<FrameGrabber*>(user)->pushFrame(frame);
    }
}

void FrameGrabber::pushFrame(MV_FRAME_OUT* frame)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_hasPending)
    {
        // A newer frame supersedes the unconsumed pending one; return its node.
        // Callbacks are serialised on one SDK thread, so this frees the previous
        // node after its callback already returned (the sanctioned free timing).
        m_camera.freeFrame(m_pending);
        m_dropped.fetch_add(1);
    }
    m_pending = *frame;   // shallow copy: the pixel node stays SDK-owned
    m_hasPending = true;
    m_received.fetch_add(1);
    m_cv.notify_one();
}

FrameView FrameGrabber::waitForFrame(unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mtx);

    // Return the node handed out on the previous call; the consumer is done.
    if (m_hasHeld)
    {
        m_camera.freeFrame(m_held);
        m_hasHeld = false;
    }

    m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                  [this] { return m_hasPending || !m_running.load(); });

    if (!m_hasPending)
    {
        return FrameView{};   // timed out or stopped
    }

    // Promote pending -> held; ownership of the SDK node moves to 'held'.
    m_held = m_pending;
    m_hasHeld = true;
    m_hasPending = false;

    m_lastWidth     = m_held.stFrameInfo.nExtendWidth;
    m_lastHeight    = m_held.stFrameInfo.nExtendHeight;
    m_lastPixelType = static_cast<int>(m_held.stFrameInfo.enPixelType);

    return makeView(m_held);
}

void FrameGrabber::stop()
{
    m_running.store(false);
    m_cv.notify_all();

    if (m_camera.isOpen())
    {
        m_camera.stopGrabbing();
        MV_CC_RegisterImageCallBackEx2(m_camera.handle(), nullptr, nullptr, false);
    }

    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_hasPending)
    {
        m_camera.freeFrame(m_pending);
        m_hasPending = false;
    }
    if (m_hasHeld)
    {
        m_camera.freeFrame(m_held);
        m_hasHeld = false;
    }
}

void FrameGrabber::close()
{
    stop();
    m_camera.close();
}

unsigned int FrameGrabber::width() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_lastWidth;
}

unsigned int FrameGrabber::height() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_lastHeight;
}

int FrameGrabber::pixelType() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_lastPixelType;
}

bool FrameGrabber::setRoi(const Roi& roi)
{
    if (!m_camera.isOpen())
    {
        return false;
    }
    // ROI can only be changed while stopped; bounce the stream if it is running.
    const bool wasRunning = m_running.load();
    if (wasRunning)
    {
        stop();
    }
    const bool ok = m_camera.setRoi(roi);
    if (wasRunning)
    {
        start();
    }
    return ok;
}

} // namespace hik
