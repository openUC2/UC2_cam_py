#ifndef UC2HIK_FRAMEGRABBER_H
#define UC2HIK_FRAMEGRABBER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "MvsCamera.h"

namespace hik {

/// Zero-copy description of the frame currently held by a FrameGrabber.
///
/// The `data` pointer aliases an SDK-owned buffer node; it stays valid only
/// until the next FrameGrabber::waitForFrame() call (or stop()/close()). Copy
/// the bytes out before then if you need to retain them.
struct FrameView
{
    bool               valid = false;        ///< false when a wait timed out or the grabber stopped
    const uint8_t*     data  = nullptr;       ///< pointer to the pixel bytes (SDK-owned, do not free)
    uint64_t           nbytes = 0;            ///< number of valid pixel bytes at `data`
    unsigned int       width = 0;             ///< image width in pixels
    unsigned int       height = 0;            ///< image height in pixels
    int                pixelType = 0;         ///< MvGvspPixelType value of the frame
    unsigned int       frameNum = 0;          ///< device frame/sequence number
    int64_t            hostTimestamp = 0;     ///< host-generated timestamp
    unsigned int       lostPacket = 0;        ///< packets lost while transferring this frame
};

/// Derive the numpy-friendly layout of a pixel format.
///
/// @param pixelType MvGvspPixelType value (as int) from a frame.
/// @param channels  [out] number of colour channels (1 for mono, 3 for RGB/BGR).
/// @param itemsize  [out] size in bytes of one channel sample (1 for 8-bit formats).
/// @param simple    [out] true if the format maps to a dense (H, W[, C]) uint8
///                  array; false if the caller should fall back to a flat byte view.
void pixelInfo(int pixelType, int& channels, int& itemsize, bool& simple);

/// Continuous, callback-driven grabber that exposes the latest frame zero-copy.
///
/// Acquisition runs on an SDK-internal worker thread (registered via
/// MV_CC_RegisterImageCallBackEx2), using the OneByOne grab strategy. The most
/// recent frame is handed to the consumer through waitForFrame(); there is no
/// application-side ring buffer here (that is a separate, external component).
///
/// Lifecycle: open() -> start() -> waitForFrame()* -> stop() -> close().
/// The class is not copyable; it owns exactly one camera.
class FrameGrabber
{
public:
    FrameGrabber();
    ~FrameGrabber();

    FrameGrabber(const FrameGrabber&) = delete;
    FrameGrabber& operator=(const FrameGrabber&) = delete;

    /// Enumerate devices and open the one at @p index. Reads PayloadSize.
    /// @param index zero-based device index into the current enumeration.
    /// @return true if the device was opened (or was already open).
    bool open(unsigned int index);

    /// Set the SDK internal buffer-node count used from the next start().
    /// @param n number of nodes (clamped to at least 3 for double buffering).
    void setNodeNum(unsigned int n);

    /// @return the configured SDK buffer-node count.
    unsigned int nodeNum() const { return m_nodeNum; }

    /// Configure the node count and grab strategy, register the image callback
    /// and begin grabbing. Must be called after open().
    /// @return true on success.
    bool start();

    /// Block until the next frame is ready, a timeout elapses, or the grabber
    /// stops. Releases the node handed out by the previous call first.
    /// @param timeoutMs maximum time to wait, in milliseconds.
    /// @return a valid FrameView, or an invalid one on timeout/stop.
    FrameView waitForFrame(unsigned int timeoutMs);

    /// Stop grabbing, unregister the callback and release any held nodes.
    void stop();

    /// stop() and then close the device.
    void close();

    /// @return true while grabbing is active.
    bool isRunning() const { return m_running.load(); }

    /// @return total frames delivered by the SDK callback so far.
    uint64_t framesReceived() const { return m_received.load(); }

    /// @return frames superseded before the consumer read them.
    uint64_t framesDropped() const { return m_dropped.load(); }

    /// @return the camera PayloadSize in bytes (0 until open()).
    uint64_t payloadSize() const { return m_payloadSize; }

    /// @return width of the most recently delivered frame (0 if none yet).
    unsigned int width() const;

    /// @return height of the most recently delivered frame (0 if none yet).
    unsigned int height() const;

    /// @return MvGvspPixelType of the most recently delivered frame (0 if none).
    int pixelType() const;

    // --- Camera parameters (delegate to the owned camera; device must be open).
    // Exposure/gain/frame-rate may be changed while grabbing; ROI cannot, so
    // setRoi() transparently stops and restarts the stream when needed.

    bool setExposureTime(double microseconds) { return m_camera.setExposureTime(microseconds); }
    bool getExposureTime(double& microseconds) { return m_camera.getExposureTime(microseconds); }
    bool getExposureTimeRange(double& minUs, double& maxUs) { return m_camera.getExposureTimeRange(minUs, maxUs); }

    bool setGain(double db) { return m_camera.setGain(db); }
    bool getGain(double& db) { return m_camera.getGain(db); }
    bool getGainRange(double& minDb, double& maxDb) { return m_camera.getGainRange(minDb, maxDb); }

    bool setFrameRate(double fps) { return m_camera.setFrameRate(fps); }
    bool getFrameRate(double& fps) { return m_camera.getFrameRate(fps); }
    bool getResultingFrameRate(double& fps) { return m_camera.getResultingFrameRate(fps); }
    bool getFrameRateRange(double& minFps, double& maxFps) { return m_camera.getFrameRateRange(minFps, maxFps); }

    bool setExposureAuto(AutoMode mode) { return m_camera.setExposureAuto(mode); }
    bool getExposureAuto(AutoMode& mode) { return m_camera.getExposureAuto(mode); }
    bool setGainAuto(AutoMode mode) { return m_camera.setGainAuto(mode); }
    bool getGainAuto(AutoMode& mode) { return m_camera.getGainAuto(mode); }

    /// Set the region of interest. If grabbing is active this stops the stream,
    /// applies the ROI and restarts it (a brief interruption), because the SDK
    /// only accepts ROI changes while stopped.
    /// @param roi requested width/height/offsetX/offsetY in pixels.
    /// @return true on success.
    bool setRoi(const Roi& roi);
    bool getRoi(Roi& out) { return m_camera.getRoi(out); }
    bool getSensorSize(int64_t& widthMax, int64_t& heightMax) { return m_camera.getSensorSize(widthMax, heightMax); }

private:
    /// SDK image callback trampoline (runs on the SDK worker thread).
    static void __stdcall onFrame(MV_FRAME_OUT* pstFrame, void* pUser, bool bAutoFree);

    /// Store a freshly delivered frame as the pending one, freeing any previous
    /// unconsumed pending node. Called from onFrame() on the SDK thread.
    void pushFrame(MV_FRAME_OUT* frame);

    MvsCamera                m_camera;
    mutable std::mutex       m_mtx;
    std::condition_variable  m_cv;

    MV_FRAME_OUT             m_pending;      ///< latest delivered frame, not yet consumed
    bool                     m_hasPending;
    MV_FRAME_OUT             m_held;         ///< frame currently exposed to the consumer
    bool                     m_hasHeld;

    std::atomic<bool>        m_running;
    std::atomic<uint64_t>    m_received;
    std::atomic<uint64_t>    m_dropped;

    unsigned int             m_nodeNum;      ///< SDK buffer-node count (>= 3)
    uint64_t                 m_payloadSize;  ///< bytes per frame reported by the camera

    unsigned int             m_lastWidth;    ///< cached geometry of the last frame
    unsigned int             m_lastHeight;
    int                      m_lastPixelType;
};

} // namespace hik

#endif // HIKCAMTEST_FRAMEGRABBER_H
