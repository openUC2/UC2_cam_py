#ifndef UC2HIK_CAMERABASE_H
#define UC2HIK_CAMERABASE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "UC2_RingBuff.h"

/// @file CameraBase.h
/// @brief Vendor-neutral camera core: settings in interface units and the
///        acquisition path into the ring buffer.
///
/// A concrete device (HikCamera, SimCamera) implements the protected `do*`
/// hooks; CameraBase owns everything that is the same for every vendor:
///
///  - the ring buffer, recreated on every start() so its geometry always
///    matches the device (ROI, binning and pixel format freeze it),
///  - the producer path: the device calls deliverFrame() from its own thread,
///    which performs the one permitted copy into a ring slot,
///  - restarting a running acquisition when ROI or binning change,
///  - zero-copy reads (readLatest / readSlot) that keep the ring alive through
///    a shared_ptr, so a reader can never outlive the memory it points into.
///
/// Units at this interface: exposure in milliseconds, frame rate in frames per
/// second, gain in dB, ROI in UNBINNED sensor pixels. Each device converts to
/// whatever its firmware uses.

namespace hik {

// ---------------------------------------------------------------------------
// Pixel formats (GenICam PFNC codes; identical to the MVS PixelType_Gvsp_*)
// ---------------------------------------------------------------------------

namespace pfnc {
static const int64_t Mono8          = 0x01080001;
static const int64_t Mono10         = 0x01100003;
static const int64_t Mono10Packed   = 0x010C0004;
static const int64_t Mono12         = 0x01100005;
static const int64_t Mono12Packed   = 0x010C0006;
static const int64_t Mono16         = 0x01100007;
static const int64_t BayerGR8       = 0x01080008;
static const int64_t BayerRG8       = 0x01080009;
static const int64_t BayerGB8       = 0x0108000A;
static const int64_t BayerBG8       = 0x0108000B;
static const int64_t BayerGR10      = 0x0110000C;
static const int64_t BayerBG10      = 0x0110000F;
static const int64_t BayerGR12      = 0x01100010;
static const int64_t BayerBG12      = 0x01100013;
static const int64_t BayerGR10Packed = 0x010C0026;
static const int64_t BayerBG12Packed = 0x010C002D;
static const int64_t BayerGR16      = 0x0110002E;
static const int64_t BayerBG16      = 0x01100031;
static const int64_t RGB8           = 0x02180014;
static const int64_t BGR8           = 0x02180015;
static const int64_t YUV422_8_UYVY  = 0x0210001F;
static const int64_t YUV422_8       = 0x02100032;
} // namespace pfnc

/// How one pixel format is stored in the ring buffer and exposed to numpy.
struct PixelLayout
{
    bool        supported = false;              ///< false: cannot be stored as is
    uint32_t    channels  = 0;                  ///< 1 (mono / raw Bayer) or 3 (RGB/BGR)
    PixelDepth  depth     = PixelDepth::UInt8;  ///< sample type of one channel
    const char* name      = "unknown";          ///< e.g. "Mono12", "BayerRG8"
};

/// @return the ring-buffer layout of PFNC pixel format @p pixelFormat.
///         Mono8/Bayer*8 -> 1 x uint8, Mono10/12/16 and Bayer*10/12/16
///         (unpacked, 16-bit containers) -> 1 x uint16, RGB8/BGR8 -> 3 x uint8.
///         Packed and YUV formats are reported as unsupported.
PixelLayout pixelLayout(int64_t pixelFormat);

/// @return supported formats a device may be switched to instead of the
///         unsupported @p pixelFormat, best first (e.g. Mono12Packed ->
///         {Mono12}, YUV422 -> {RGB8, BGR8}). Empty if there is none.
std::vector<int64_t> unpackedAlternatives(int64_t pixelFormat);

// ---------------------------------------------------------------------------
// Value helpers (shared by all devices)
// ---------------------------------------------------------------------------

/// Snap @p value to the grid min + k*inc inside [min, max], rounding to the
/// NEAREST grid point (ties go up) - unlike alignRoiValue(), which rounds down.
int64_t nearestOnGrid(int64_t value, int64_t min, int64_t max, int64_t inc);

/// @return the entry of @p supported closest to @p requested (ties go to the
///         smaller factor); 1 if @p supported is empty.
uint32_t nearestBinning(uint32_t requested, const std::vector<uint32_t>& supported);

/// @return @p value clamped to [@p lo, @p hi].
double clampValue(double value, double lo, double hi);

/// Region of interest in pixels: x/y offset from the top-left corner plus size.
struct SensorRoi
{
    int64_t x      = 0;
    int64_t y      = 0;
    int64_t width  = 0;
    int64_t height = 0;
};

/// Convert an unbinned sensor ROI to binned device coordinates (floor).
SensorRoi toBinned(const SensorRoi& sensor, uint32_t binning);

/// Convert a binned device ROI back to unbinned sensor coordinates.
SensorRoi toSensor(const SensorRoi& binned, uint32_t binning);

/// A closed interval [min, max] in interface units; valid == false when the
/// device does not offer it.
struct ValueRange
{
    bool   valid = false;
    double min   = 0.0;
    double max   = 0.0;
};

// ---------------------------------------------------------------------------
// CameraBase
// ---------------------------------------------------------------------------

class CameraBase
{
public:
    /// Configuration of the acquisition buffer, applied at the next start().
    struct BufferConfig
    {
        uint32_t    slotCount = 16;   ///< number of images the ring holds (>= 2)
        std::string storagePath;      ///< directory for the .uc2rb file; empty = RAM only
        std::string comment;          ///< free text for the session header (<= 8191 chars)
    };

    /// One frame read from the ring: the pinned slot plus the ring it lives in.
    /// Member order matters: `handle` is destroyed (slot released) before `ring`.
    struct Image
    {
        std::shared_ptr<RingBuffer> ring;
        RingBuffer::ReadHandle      handle;
        UC2_SessionHeader           header;   ///< geometry of that ring
    };

    virtual ~CameraBase();

    CameraBase(const CameraBase&)            = delete;
    CameraBase& operator=(const CameraBase&) = delete;

    // --- Identity ---------------------------------------------------------
    const std::string& vendor() const         { return m_vendor; }
    const std::string& model() const          { return m_model; }
    const std::string& serial() const         { return m_serial; }
    const std::string& connectionType() const { return m_connectionType; }
    int                index() const          { return m_index; }

    // --- Capabilities (evaluated live; may change with the device state) --
    virtual bool canSetExposure()  = 0;
    virtual bool canSetGain()      = 0;
    virtual bool canSetFrameRate() = 0;
    virtual bool canSetBinning()   = 0;
    virtual bool canSetTrigger()   = 0;
    virtual bool canSetRoi()       = 0;

    // --- Ranges (interface units) -----------------------------------------
    virtual ValueRange exposureRangeMs() = 0;
    virtual ValueRange frameRateRange()  = 0;
    virtual ValueRange gainRangeDb()     = 0;
    /// Symmetric binning factors the device accepts; {1} if binning is fixed.
    virtual std::vector<uint32_t> binningValues() = 0;

    // --- Current values (false if not readable) ---------------------------
    virtual bool getExposureMs(double& ms)   = 0;
    virtual bool getFrameRate(double& fps)   = 0;
    virtual bool getGainDb(double& db)       = 0;
    virtual bool getBinning(uint32_t& factor) = 0;
    /// Current ROI in unbinned sensor pixels.
    virtual bool getRoi(SensorRoi& roi)      = 0;
    /// Full unbinned sensor resolution; false if the firmware does not tell.
    virtual bool getSensorSize(int64_t& width, int64_t& height) = 0;
    /// Size of the images the device delivers right now (binned ROI).
    virtual bool getImageSize(int64_t& width, int64_t& height) = 0;
    /// PFNC code of the current pixel format.
    virtual int64_t pixelFormat() = 0;

    // --- Setters ----------------------------------------------------------
    // Requests are clamped/snapped to what the device accepts; read the value
    // back to see what was applied. They return false when the device refused.
    virtual bool setExposureMs(double ms) = 0;
    virtual bool setExposureAuto()        = 0;   ///< firmware regulates exposure
    virtual bool setFrameRate(double fps) = 0;
    virtual bool disableFrameRateLimit()  = 0;   ///< free-run as fast as possible
    virtual bool setGainDb(double db)     = 0;
    virtual bool setGainAuto()            = 0;   ///< firmware regulates gain

    /// Set symmetric binning (snapped to binningValues()). Restarts a running
    /// acquisition, because the image size changes.
    bool setBinning(uint32_t factor);
    /// Set the ROI in unbinned sensor pixels (snapped to the device grid).
    /// Restarts a running acquisition.
    bool setRoi(const SensorRoi& roi);
    /// Full sensor (no ROI). Restarts a running acquisition.
    bool resetRoi();

    // --- Acquisition ------------------------------------------------------
    /// Buffer settings for the next start().
    void setBufferConfig(const BufferConfig& cfg);
    BufferConfig bufferConfig() const;
    /// Switch storing to disk on (directory @p dirPath) or off (empty). While
    /// running, the current ring starts or stops streaming without a restart:
    /// on opens a new .uc2rb file from the next frame, off finishes and closes
    /// it. Otherwise only BufferConfig::storagePath changes, for the next start().
    /// @return false if the file cannot be created; storing is then off.
    bool setStoragePath(const std::string& dirPath);

    /// Allocate a fresh ring buffer for the current geometry, open the session
    /// (disk streaming if a storage path is set) and start the device stream.
    /// @return false (and logged) on any failure; nothing keeps running then.
    bool start();
    /// Stop the device stream and close the session. Frames stay readable.
    void stop();
    /// stop() and release the device. Readers keep their rings alive.
    void close();
    /// @return true between a successful start() and stop().
    bool isRunning() const { return m_running.load(); }
    /// @return true if a running acquisition was restarted by the last
    ///         setRoi()/setBinning()/resetRoi() call.
    bool lastChangeRestarted() const { return m_lastRestarted; }

    // --- Reading (zero-copy) ----------------------------------------------
    /// Pin the most recently committed frame. false if there is none yet.
    bool readLatest(Image& out);
    /// Block until the current ring holds a frame newer than @p afterSequence
    /// (-1: any frame), see RingBuffer::waitForFrame(). Before the first start()
    /// there is no ring: sleeps @p timeoutMs and returns false.
    bool waitForFrame(int64_t afterSequence, uint32_t timeoutMs);
    /// Pin whatever frame occupies ring slot @p slot of the current ring.
    /// false if there is no ring yet, the index is out of range or the slot
    /// holds no complete frame.
    bool readSlot(uint32_t slot, Image& out);

    /// @return the current ring (may be null before the first start()).
    std::shared_ptr<RingBuffer> ring() const;
    /// @return true and the session header of the current ring, if any.
    bool sessionHeader(UC2_SessionHeader& out) const;
    /// @return path of the current .uc2rb file (empty if not streaming).
    std::string recordingFile() const;
    /// @return frames the device delivered but that could not be stored
    ///         (size mismatch, ring busy) since the last start().
    uint64_t framesRejected() const { return m_rejected.load(); }

protected:
    /// Derived constructors open the device first and pass what they found.
    /// Derived destructors must call close() themselves: the hooks are pure
    /// virtual, so ~CameraBase() can no longer reach them.
    CameraBase(std::string vendor, std::string model, std::string serial,
               std::string connectionType, int index);

    /// Producer entry point, called on the device's delivery thread for every
    /// frame. Performs the single copy into the ring.
    void deliverFrame(const void* data, uint64_t nbytes);

    // --- Device hooks -----------------------------------------------------
    virtual bool doSetBinning(uint32_t factor) = 0;
    virtual bool doSetRoi(const SensorRoi& roi) = 0;
    virtual bool doResetRoi() = 0;
    /// Start delivering frames through deliverFrame().
    virtual bool doStartStream() = 0;
    /// Stop delivering. After this returns no new deliverFrame() call starts.
    virtual void doStopStream() = 0;
    /// Release the device (stream already stopped).
    virtual void doClose() = 0;

private:
    bool startLocked();
    void stopLocked();
    /// Run @p change, restarting the acquisition around it when it is running.
    template <typename F> bool changeGeometry(F change);

    std::string m_vendor;
    std::string m_model;
    std::string m_serial;
    std::string m_connectionType;
    int         m_index;

    mutable std::mutex           m_mtx;          ///< serialises start/stop/geometry/config
    BufferConfig                 m_cfg;
    std::shared_ptr<RingBuffer>  m_ring;         ///< current (or last) ring
    std::atomic<RingBuffer*>     m_active;       ///< ring the producer writes to, null when stopped
    std::atomic<int>             m_inFlight;     ///< deliverFrame() calls in progress
    std::atomic<bool>            m_running;
    std::atomic<uint64_t>        m_rejected;
    std::atomic<bool>            m_sizeWarned;
    bool                         m_lastRestarted = false;
    bool                         m_closed        = false;
};

} // namespace hik

#endif // UC2HIK_CAMERABASE_H
