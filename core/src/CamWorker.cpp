#include "CamWorker.h"

#include "CamIpc.h"
#include "CameraBase.h"
#include "SimCamera.h"
#include "UC2Version.h"
#include "hikLog.h"

#if HIK_HAVE_MVS
#include "HikCamera.h"
#include "MvsCamera.h"
#endif

#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hik {

namespace {

using ipc::Capability;
using ipc::ErrorKind;
using ipc::Kind;
using ipc::Message;
using ipc::Op;
using ipc::Quantity;

/// Longest log text forwarded in one message; the rest is cut off.
const size_t kMaxLogText = 16 * 1024;

volatile sig_atomic_t g_terminate = 0;

extern "C" void onTerminate(int /*sig*/)
{
    g_terminate = 1;
}

// ---------------------------------------------------------------------------
// Log forwarding
// ---------------------------------------------------------------------------

struct LogTarget
{
    int                   sock = -1;
    std::thread::id       commandThread;
    std::atomic<uint64_t> dropped{0};
};

LogTarget g_log;

void socketSink(LogLevel level, const char* message, void* ctx)
{
    LogTarget* t = static_cast<LogTarget*>(ctx);
    std::string text(message);
    if (text.size() > kMaxLogText)
    {
        text.resize(kMaxLogText);
    }
    Message m(Kind::Event, Op::Log, 0);
    m.putU32(static_cast<uint32_t>(level)).putStr(text);
    // The command thread's messages must arrive before its response, so it
    // waits for room; the camera and writer threads never block on a log line.
    const bool dontWait = std::this_thread::get_id() != t->commandThread;
    if (!ipc::sendMessage(t->sock, m, -1, dontWait))
    {
        t->dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Request helpers
// ---------------------------------------------------------------------------

/// Thrown for a request whose fields do not match its operation.
struct MalformedRequest : std::runtime_error
{
    explicit MalformedRequest(const std::string& what) : std::runtime_error(what) {}
};

template <typename T, typename F>
T field(F get, const char* what)
{
    T v{};
    if (!get(v))
    {
        throw MalformedRequest(std::string("missing or mistyped field: ") + what);
    }
    return v;
}

uint32_t u32(Message& m, const char* what) { return field<uint32_t>([&m](uint32_t& v) { return m.getU32(v); }, what); }
int64_t  i64(Message& m, const char* what) { return field<int64_t>([&m](int64_t& v) { return m.getI64(v); }, what); }
double   f64(Message& m, const char* what) { return field<double>([&m](double& v) { return m.getF64(v); }, what); }
std::string str(Message& m, const char* what)
{
    return field<std::string>([&m](std::string& v) { return m.getStr(v); }, what);
}

uint32_t parseUnsigned(const std::string& key, const std::string& value)
{
    char* end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0' || errno != 0 || v > 0xFFFFFFFFul || value[0] == '-')
    {
        throw std::invalid_argument("SimCamera: " + key + " must be an unsigned integer, got '" + value + "'");
    }
    return static_cast<uint32_t>(v);
}

double parseReal(const std::string& key, const std::string& value)
{
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(value.c_str(), &end);
    if (value.empty() || *end != '\0' || errno != 0)
    {
        throw std::invalid_argument("SimCamera: " + key + " must be a number, got '" + value + "'");
    }
    return v;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

class Worker
{
public:
    explicit Worker(int sock) : m_sock(sock) {}

    int run();

private:
    /// Execute @p req and send its response. @return false when the process
    /// should end (after Close).
    bool handle(Message& req);
    void open(Message& req);
    void announceRing();
    /// Response to @p req: success, the acquisition state, then the payload
    /// @p fill appends.
    template <typename F>
    void reply(const Message& req, F fill);
    void replyError(const Message& req, ErrorKind kind, const std::string& text);
    CameraBase& cam();
    SimCamera&  sim();
    void closeCamera();

    int                          m_sock;
    std::shared_ptr<CameraBase>  m_cam;
    SimCamera*                   m_sim = nullptr;
    bool                         m_sdk = false;
    std::shared_ptr<RingBuffer>  m_announced;      ///< last ring sent to the application
    int64_t                      m_generation = 0;
};

CameraBase& Worker::cam()
{
    if (!m_cam)
    {
        throw std::runtime_error("no camera is open in this process");
    }
    return *m_cam;
}

SimCamera& Worker::sim()
{
    if (m_sim == nullptr)
    {
        throw std::runtime_error("only available for the Simulated vendor");
    }
    return *m_sim;
}

template <typename F>
void Worker::reply(const Message& req, F fill)
{
    // A new session's ring must be known on the other side before the call
    // that created it (start, ROI, binning) returns there.
    announceRing();
    Message m(Kind::Response, req.op(), req.id());
    m.putU32(static_cast<uint32_t>(ErrorKind::None));
    m.putFlag(m_cam && m_cam->isRunning());
    m.putFlag(m_cam && m_cam->lastChangeRestarted());
    fill(m);
    ipc::sendMessage(m_sock, m);
}

void Worker::replyError(const Message& req, ErrorKind kind, const std::string& text)
{
    Message m(Kind::Response, req.op(), req.id());
    m.putU32(static_cast<uint32_t>(kind));
    m.putStr(text);
    ipc::sendMessage(m_sock, m);
}

void Worker::announceRing()
{
    if (!m_cam)
    {
        return;
    }
    std::shared_ptr<RingBuffer> r = m_cam->ring();
    if (!r || r == m_announced || r->sharedFd() < 0)
    {
        return;
    }
    Message m(Kind::Event, Op::Ring, 0);
    m.putI64(++m_generation);
    if (!ipc::sendMessage(m_sock, m, r->sharedFd()))
    {
        ERR("uc2_cam_worker: cannot hand the ring buffer to the application: "
            + std::string(std::strerror(errno)));
        return;
    }
    m_announced = r;
}

void Worker::open(Message& req)
{
    if (m_cam)
    {
        throw std::runtime_error("a camera is already open in this process");
    }
    const std::string vendor     = str(req, "vendor");
    const std::string connection = str(req, "connection type");
    const std::string serial     = str(req, "serial");
    const int64_t     index      = i64(req, "index");
    const uint32_t    level      = u32(req, "log level");
    const uint32_t    n          = u32(req, "option count");
    std::vector<std::pair<std::string, std::string>> options;
    for (uint32_t i = 0; i < n; ++i)
    {
        std::string key   = str(req, "option key");
        std::string value = str(req, "option value");
        options.emplace_back(std::move(key), std::move(value));
    }
    setLogLevel(static_cast<LogLevel>(level));

    if (vendor == "HikRobot")
    {
#if HIK_HAVE_MVS
        if (!options.empty())
        {
            throw std::invalid_argument("HikCamera: unexpected option '" + options.front().first + "'");
        }
        if (!MvsCamera::initSdk())
        {
            throw std::runtime_error("HikCamera: MVS SDK initialisation failed");
        }
        m_sdk = true;
        m_cam = std::make_shared<HikCamera>(connection, serial, static_cast<int>(index));
#else
        (void)index;   // only HikRobot selects by index
        throw std::runtime_error("HikRobot cameras are not available: uc2_cam_py was built without the MVS SDK");
#endif
    }
    else if (vendor == "Simulated")
    {
        SimCamera::Options o;
        if (!serial.empty())     { o.serial = serial; }
        if (!connection.empty()) { o.connectionType = connection; }
        for (const auto& kv : options)
        {
            if (kv.first == "pixel_format")
            {
                if (!SimCamera::pixelFormatByName(kv.second, o.pixelFormat))
                {
                    throw std::invalid_argument("SimCamera: pixel_format must be 'Mono8' or 'Mono16'");
                }
            }
            else if (kv.first == "sensor_width")  { o.sensorWidth  = parseUnsigned(kv.first, kv.second); }
            else if (kv.first == "sensor_height") { o.sensorHeight = parseUnsigned(kv.first, kv.second); }
            else if (kv.first == "frame_rate")    { o.frameRate    = parseReal(kv.first, kv.second); }
            else
            {
                throw std::invalid_argument("SimCamera: unexpected option '" + kv.first + "'");
            }
        }
        auto s = std::make_shared<SimCamera>(o);
        m_sim = s.get();
        m_cam = s;
    }
    else
    {
        throw std::invalid_argument("unknown vendor '" + vendor + "'");
    }

    // Visible in ps/top/htop: which process serves which camera.
    const std::string name = ("uc2cam-" + m_cam->serial()).substr(0, 15);
    ::prctl(PR_SET_NAME, name.c_str(), 0, 0, 0);
    DBG("uc2_cam_worker " + std::to_string(::getpid()) + ": serving " + m_cam->vendor() + " "
        + m_cam->model() + " " + m_cam->serial());

    reply(req, [this](Message& m) {
        m.putStr(m_cam->vendor()).putStr(m_cam->model()).putStr(m_cam->serial())
         .putStr(m_cam->connectionType()).putI64(m_cam->index());
    });
}

bool Worker::handle(Message& req)
{
    switch (req.op())
    {
    case Op::Open:
        open(req);
        return true;

    case Op::CanSet:
    {
        const uint32_t what = u32(req, "capability");
        bool v = false;
        switch (static_cast<Capability>(what))
        {
        case Capability::Exposure:  v = cam().canSetExposure();  break;
        case Capability::Gain:      v = cam().canSetGain();      break;
        case Capability::FrameRate: v = cam().canSetFrameRate(); break;
        case Capability::Binning:   v = cam().canSetBinning();   break;
        case Capability::Trigger:   v = cam().canSetTrigger();   break;
        case Capability::Roi:       v = cam().canSetRoi();       break;
        default: throw MalformedRequest("unknown capability " + std::to_string(what));
        }
        reply(req, [v](Message& m) { m.putFlag(v); });
        return true;
    }

    case Op::Range:
    {
        const uint32_t what = u32(req, "quantity");
        ValueRange r;
        switch (static_cast<Quantity>(what))
        {
        case Quantity::ExposureMs: r = cam().exposureRangeMs(); break;
        case Quantity::FrameRate:  r = cam().frameRateRange();  break;
        case Quantity::GainDb:     r = cam().gainRangeDb();     break;
        default: throw MalformedRequest("unknown quantity " + std::to_string(what));
        }
        reply(req, [&r](Message& m) { m.putFlag(r.valid).putF64(r.min).putF64(r.max); });
        return true;
    }

    case Op::BinningValues:
    {
        const std::vector<uint32_t> v = cam().binningValues();
        reply(req, [&v](Message& m) {
            m.putU32(static_cast<uint32_t>(v.size()));
            for (uint32_t b : v) { m.putU32(b); }
        });
        return true;
    }

    case Op::GetValue:
    {
        const uint32_t what = u32(req, "quantity");
        double v  = 0.0;
        bool   ok = false;
        switch (static_cast<Quantity>(what))
        {
        case Quantity::ExposureMs: ok = cam().getExposureMs(v); break;
        case Quantity::FrameRate:  ok = cam().getFrameRate(v);  break;
        case Quantity::GainDb:     ok = cam().getGainDb(v);     break;
        default: throw MalformedRequest("unknown quantity " + std::to_string(what));
        }
        reply(req, [ok, v](Message& m) { m.putFlag(ok).putF64(v); });
        return true;
    }

    case Op::GetBinning:
    {
        uint32_t v = 0;
        const bool ok = cam().getBinning(v);
        reply(req, [ok, v](Message& m) { m.putFlag(ok).putU32(v); });
        return true;
    }

    case Op::GetRoi:
    {
        SensorRoi r;
        const bool ok = cam().getRoi(r);
        reply(req, [ok, &r](Message& m) { m.putFlag(ok).putI64(r.x).putI64(r.y).putI64(r.width).putI64(r.height); });
        return true;
    }

    case Op::GetSensorSize:
    case Op::GetImageSize:
    {
        int64_t w = 0, h = 0;
        const bool ok = (req.op() == Op::GetSensorSize) ? cam().getSensorSize(w, h) : cam().getImageSize(w, h);
        reply(req, [ok, w, h](Message& m) { m.putFlag(ok).putI64(w).putI64(h); });
        return true;
    }

    case Op::PixelFormat:
    {
        const int64_t pf = cam().pixelFormat();
        reply(req, [pf](Message& m) { m.putI64(pf); });
        return true;
    }

    case Op::SetValue:
    {
        const uint32_t what = u32(req, "quantity");
        const double   v    = f64(req, "value");
        bool ok = false;
        switch (static_cast<Quantity>(what))
        {
        case Quantity::ExposureMs: ok = cam().setExposureMs(v); break;
        case Quantity::FrameRate:  ok = cam().setFrameRate(v);  break;
        case Quantity::GainDb:     ok = cam().setGainDb(v);     break;
        default: throw MalformedRequest("unknown quantity " + std::to_string(what));
        }
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::SetAuto:
    {
        const uint32_t what = u32(req, "quantity");
        bool ok = false;
        switch (static_cast<Quantity>(what))
        {
        case Quantity::ExposureMs: ok = cam().setExposureAuto(); break;
        case Quantity::GainDb:     ok = cam().setGainAuto();     break;
        default: throw MalformedRequest("no automatic mode for quantity " + std::to_string(what));
        }
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::DisableFrameRateLimit:
    {
        const bool ok = cam().disableFrameRateLimit();
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::SetBinning:
    {
        const bool ok = cam().setBinning(u32(req, "factor"));
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::SetRoi:
    {
        SensorRoi r;
        r.x      = i64(req, "x");
        r.y      = i64(req, "y");
        r.width  = i64(req, "width");
        r.height = i64(req, "height");
        const bool ok = cam().setRoi(r);
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::ResetRoi:
    {
        const bool ok = cam().resetRoi();
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::SetBufferConfig:
    {
        CameraBase::BufferConfig c;
        c.slotCount    = u32(req, "slot count");
        c.storagePath  = str(req, "storage path");
        c.comment      = str(req, "comment");
        c.sharedMemory = true;   // the application reads the ring from this process
        cam().setBufferConfig(c);
        reply(req, [](Message&) {});
        return true;
    }

    case Op::SetStoragePath:
    {
        const bool ok = cam().setStoragePath(str(req, "path"));
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::Start:
    {
        const bool ok = cam().start();
        reply(req, [ok](Message& m) { m.putFlag(ok); });
        return true;
    }

    case Op::Stop:
        cam().stop();
        reply(req, [](Message&) {});
        return true;

    case Op::Close:
        closeCamera();
        reply(req, [](Message&) {});
        return false;

    case Op::RecordingFile:
    {
        const std::string path = cam().recordingFile();
        reply(req, [&path](Message& m) { m.putStr(path); });
        return true;
    }

    case Op::FramesRejected:
    {
        const uint64_t n = cam().framesRejected();
        reply(req, [n](Message& m) { m.putI64(static_cast<int64_t>(n)); });
        return true;
    }

    case Op::StreamStats:
    {
        uint64_t saved = 0, dropped = 0;
        bool error = false;
        cam().streamStats(saved, dropped, error);
        reply(req, [=](Message& m) {
            m.putI64(static_cast<int64_t>(saved)).putI64(static_cast<int64_t>(dropped)).putFlag(error);
        });
        return true;
    }

    case Op::SimSpotPosition:
    {
        const sim::SpotPosition p = sim().spotPositionFor(static_cast<uint64_t>(i64(req, "sequence")));
        reply(req, [&p](Message& m) { m.putF64(p.x).putF64(p.y); });
        return true;
    }

    case Op::SimExposureAuto:
    {
        const bool v = sim().exposureAuto();
        reply(req, [v](Message& m) { m.putFlag(v); });
        return true;
    }

    case Op::SimGainAuto:
    {
        const bool v = sim().gainAuto();
        reply(req, [v](Message& m) { m.putFlag(v); });
        return true;
    }

    default:
        throw MalformedRequest("unknown operation " + std::to_string(static_cast<int>(req.op())));
    }
}

void Worker::closeCamera()
{
    if (m_cam)
    {
        m_cam->close();   // stops the stream and completes the recording file
    }
}

int Worker::run()
{
    g_log.sock          = m_sock;
    g_log.commandThread = std::this_thread::get_id();
    setLogSink(&socketSink, &g_log);

    Message hello(Kind::Event, Op::Hello, 0);
    hello.putU32(ipc::kProtocolVersion).putI64(static_cast<int64_t>(::getpid())).putStr(version());
    if (!ipc::sendMessage(m_sock, hello))
    {
        setLogSink(nullptr, nullptr);
        return 1;   // nobody to serve
    }

    bool applicationGone = false;
    while (g_terminate == 0)
    {
        Message req;
        int fd = -1;
        std::string why;
        const ipc::RecvStatus st = ipc::recvMessage(m_sock, req, fd, 200, why);
        if (st == ipc::RecvStatus::Timeout)
        {
            continue;
        }
        if (st == ipc::RecvStatus::Closed || st == ipc::RecvStatus::Error)
        {
            applicationGone = true;
            break;
        }
        if (fd >= 0)
        {
            ::close(fd);   // requests never carry a descriptor
        }
        if (st == ipc::RecvStatus::Malformed)
        {
            ERR("uc2_cam_worker: malformed message ignored: " + why);
            continue;
        }
        if (req.kind() != Kind::Request)
        {
            WARN("uc2_cam_worker: ignoring a message that is not a request");
            continue;
        }

        const uint64_t dropped = g_log.dropped.exchange(0, std::memory_order_relaxed);
        if (dropped != 0)
        {
            WARN("uc2_cam_worker: " + std::to_string(dropped) + " log message(s) dropped (socket full)");
        }

        bool keepGoing = true;
        try
        {
            keepGoing = handle(req);
        }
        catch (const MalformedRequest& e)
        {
            replyError(req, ErrorKind::Runtime, std::string("malformed request: ") + e.what());
        }
        catch (const std::invalid_argument& e)
        {
            replyError(req, ErrorKind::InvalidArgument, e.what());
        }
        catch (const std::out_of_range& e)
        {
            replyError(req, ErrorKind::OutOfRange, e.what());
        }
        catch (const std::exception& e)
        {
            replyError(req, ErrorKind::Runtime, e.what());
        }
        if (!keepGoing)
        {
            break;
        }
    }

    if (applicationGone)
    {
        // Nobody listens any more: keep only what matters on stderr.
        setLogSink(nullptr, nullptr);
        setLogLevel(LogLevel::Warning);
    }
    closeCamera();
    m_cam.reset();
    m_sim = nullptr;
    m_announced.reset();
    setLogSink(nullptr, nullptr);
#if HIK_HAVE_MVS
    if (m_sdk)
    {
        MvsCamera::finalizeSdk();
    }
#endif
    return 0;
}

} // namespace

int runCameraWorker(int sock)
{
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &onTerminate;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);

    Worker w(sock);
    return w.run();
}

} // namespace hik
