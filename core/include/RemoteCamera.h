#ifndef UC2HIK_REMOTECAMERA_H
#define UC2HIK_REMOTECAMERA_H

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "CamIpc.h"
#include "CameraBase.h"
#include "UC2_RingBuff.h"

/// @file RemoteCamera.h
/// @brief A camera that runs in its own process (uc2_cam_worker).
///
/// RemoteCamera offers the same calls as CameraBase, with the same signatures,
/// but the device, the acquisition thread and the disk writer live in a camera
/// process of their own. Settings travel over a Unix socket; frames do not:
/// every session's ring buffer sits in a sealed memfd that the camera process
/// hands over, and RemoteCamera maps it with RingBuffer::attach(). readLatest(),
/// readSlot() and waitForFrame() therefore read shared memory directly, zero-copy
/// and without a round trip, exactly like the in-process path.
///
/// If the camera process dies or stops answering, the camera is "lost": one
/// error is logged, every further call fails (false / empty), and isRunning()
/// is false. Images already read stay valid -- their mapping belongs to this
/// process. Nothing the camera process does can make this process crash.

namespace hik {

class RemoteCamera
{
public:
    /// Vendor-specific options as text, e.g. {"frame_rate", "100"}.
    using Options = std::vector<std::pair<std::string, std::string>>;

    /// Start @p workerPath and open a camera in it.
    /// @param vendor "HikRobot" or "Simulated".
    /// @throws std::invalid_argument, std::out_of_range, std::runtime_error as
    ///         the in-process constructors do (bad arguments, index out of
    ///         range, no such camera); std::runtime_error also if the camera
    ///         process cannot be started.
    RemoteCamera(const std::string& workerPath, const std::string& vendor,
                 const std::string& connectionType, const std::string& serial, int index,
                 const Options& options);
    /// close()s the camera and collects the process.
    ~RemoteCamera();

    RemoteCamera(const RemoteCamera&)            = delete;
    RemoteCamera& operator=(const RemoteCamera&) = delete;

    // --- Identity ---------------------------------------------------------
    const std::string& vendor() const         { return m_vendor; }
    const std::string& model() const          { return m_model; }
    const std::string& serial() const         { return m_serial; }
    const std::string& connectionType() const { return m_connectionType; }
    int                index() const          { return m_index; }

    // --- The CameraBase calls ----------------------------------------------
    bool canSetExposure();
    bool canSetGain();
    bool canSetFrameRate();
    bool canSetBinning();
    bool canSetTrigger();
    bool canSetRoi();

    ValueRange exposureRangeMs();
    ValueRange frameRateRange();
    ValueRange gainRangeDb();
    std::vector<uint32_t> binningValues();

    bool getExposureMs(double& ms);
    bool getFrameRate(double& fps);
    bool getGainDb(double& db);
    bool getBinning(uint32_t& factor);
    bool getRoi(SensorRoi& roi);
    bool getSensorSize(int64_t& width, int64_t& height);
    bool getImageSize(int64_t& width, int64_t& height);
    int64_t pixelFormat();

    bool setExposureMs(double ms);
    bool setExposureAuto();
    bool setFrameRate(double fps);
    bool disableFrameRateLimit();
    bool setGainDb(double db);
    bool setGainAuto();

    bool setBinning(uint32_t factor);
    bool setRoi(const SensorRoi& roi);
    bool resetRoi();
    bool lastChangeRestarted() const { return m_lastRestarted.load(); }

    void setBufferConfig(const CameraBase::BufferConfig& cfg);
    CameraBase::BufferConfig bufferConfig() const;
    bool setStoragePath(const std::string& dirPath);

    bool start();
    void stop();
    /// Close the camera, let the camera process end and collect it (killed
    /// after 2 s if it does not end by itself). Idempotent.
    void close();
    bool isRunning() const { return m_running.load(); }

    bool readLatest(CameraBase::Image& out);
    bool waitForFrame(int64_t afterSequence, uint32_t timeoutMs);
    bool readSlot(uint32_t slot, CameraBase::Image& out);

    std::shared_ptr<RingBuffer> ring() const;
    bool sessionHeader(UC2_SessionHeader& out) const;
    std::string recordingFile();
    uint64_t framesRejected();
    void streamStats(uint64_t& saved, uint64_t& dropped, bool& error);

    // --- Simulated camera extras (std::runtime_error for other vendors) ----
    bool spotPositionFor(uint64_t sequence, double& x, double& y);
    bool exposureAuto();
    bool gainAuto();

    // --- The camera process -------------------------------------------------
    /// @return process id of the camera process.
    pid_t pid() const { return m_pid; }
    /// @return true once the camera process died or stopped answering.
    bool isLost() const;
    /// @return the tag its log messages carry (see ScopedLogSource); unique per instance.
    int logSource() const { return m_source; }

private:
    struct Reply
    {
        ipc::Message   msg;   ///< positioned at the payload on success
        ipc::ErrorKind error = ipc::ErrorKind::None;
        std::string    text;  ///< error text
    };

    void spawn(const std::string& workerPath);
    ipc::Message newRequest(ipc::Op op);
    /// Send @p req and wait up to @p timeoutMs for its response.
    /// @return true for a success response; false if the camera is lost, does
    ///         not answer in time, or answered with an error (in @p out).
    bool call(ipc::Message& req, Reply& out, int timeoutMs);
    /// call() for operations answered with one flag; false on any failure.
    bool callFlag(ipc::Message req, int timeoutMs);
    /// call() whose error responses are logged (all but Open).
    bool callLogged(ipc::Message& req, Reply& out, int timeoutMs);
    bool callValue(ipc::Quantity q, double& v);
    bool callSize(ipc::Op op, int64_t& w, int64_t& h);
    ValueRange callRange(ipc::Quantity q);
    void receiveLoop();
    void handleEvent(ipc::Message& msg, int fd);
    /// Declare the camera lost (once): fail all waiting and future calls, log why.
    void markLost(const std::string& reason);
    /// Collect the camera process; wait up to @p timeoutMs (-1: for ever).
    /// @return true once it has been collected.
    bool reap(int timeoutMs);
    void shutdownProcess();
    std::string exitDescription();

    std::string m_vendor;
    std::string m_model;
    std::string m_serial;
    std::string m_connectionType;
    int         m_index  = -1;
    int         m_source = 0;

    int         m_sock = -1;
    pid_t       m_pid  = -1;
    std::thread m_receiver;

    mutable std::mutex                m_mtx;        ///< everything below up to m_ring
    std::condition_variable           m_cv;
    uint32_t                          m_nextId = 1;
    std::set<uint32_t>                m_waiting;    ///< ids of calls waiting for a response
    std::map<uint32_t, ipc::Message>  m_replies;    ///< responses not yet picked up
    bool                              m_hello   = false;
    std::string                       m_workerVersion;   ///< from the Hello, "" if it had none
    bool                              m_lost    = false;
    bool                              m_closing = false;
    std::shared_ptr<RingBuffer>       m_ring;       ///< attached ring of the current session
    CameraBase::BufferConfig          m_cfg;

    std::mutex        m_closeMtx;   ///< serialises close()
    bool              m_closed = false;
    std::mutex        m_reapMtx;    ///< guards m_reaped / m_status
    bool              m_reaped = false;
    int               m_status = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_lastRestarted{false};
};

} // namespace hik

#endif // UC2HIK_REMOTECAMERA_H
