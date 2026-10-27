#include "RemoteCamera.h"

#include "hikLog.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

extern char** environ;

namespace hik {

namespace {

using ipc::Kind;
using ipc::Message;
using ipc::Op;

/// Answer time for calls that only read or write a setting.
const int kFastMs = 5000;
/// Answer time for calls that open the device or (re)start a session.
const int kSlowMs = 30000;
/// How long close() waits for the camera process to end before killing it.
const int kExitMs = 2000;

std::atomic<int> g_nextSource(1);

const char* opName(Op op)
{
    switch (op)
    {
    case Op::Open:                  return "open";
    case Op::CanSet:                return "capability query";
    case Op::Range:                 return "range query";
    case Op::BinningValues:         return "binning query";
    case Op::GetValue:              return "value query";
    case Op::GetBinning:            return "binning query";
    case Op::GetRoi:                return "ROI query";
    case Op::GetSensorSize:         return "sensor size query";
    case Op::GetImageSize:          return "image size query";
    case Op::PixelFormat:           return "pixel format query";
    case Op::SetValue:              return "setting";
    case Op::SetAuto:               return "auto setting";
    case Op::DisableFrameRateLimit: return "frame rate setting";
    case Op::SetBinning:            return "binning setting";
    case Op::SetRoi:                return "ROI setting";
    case Op::ResetRoi:              return "ROI reset";
    case Op::SetBufferConfig:       return "buffer setting";
    case Op::SetStoragePath:        return "storage setting";
    case Op::Start:                 return "start";
    case Op::Stop:                  return "stop";
    case Op::Close:                 return "close";
    case Op::RecordingFile:         return "recording file query";
    case Op::FramesRejected:        return "statistics query";
    case Op::StreamStats:           return "statistics query";
    case Op::SimSpotPosition:       return "spot position query";
    case Op::SimExposureAuto:       return "auto query";
    case Op::SimGainAuto:           return "auto query";
    default:                        return "request";
    }
}

[[noreturn]] void rethrow(ipc::ErrorKind kind, const std::string& text)
{
    switch (kind)
    {
    case ipc::ErrorKind::InvalidArgument: throw std::invalid_argument(text);
    case ipc::ErrorKind::OutOfRange:      throw std::out_of_range(text);
    default:                              throw std::runtime_error(text);
    }
}

LogLevel sanitizedLevel(uint32_t level)
{
    switch (static_cast<LogLevel>(level))
    {
    case LogLevel::Debug:
    case LogLevel::Info:
    case LogLevel::Warning:
    case LogLevel::Error:
        return static_cast<LogLevel>(level);
    }
    return LogLevel::Warning;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and teardown
// ---------------------------------------------------------------------------

RemoteCamera::RemoteCamera(const std::string& workerPath, const std::string& vendor,
                           const std::string& connectionType, const std::string& serial, int index,
                           const Options& options)
    : m_source(g_nextSource.fetch_add(1))
{
    ScopedLogSource tag(m_source);
    spawn(workerPath);
    m_receiver = std::thread(&RemoteCamera::receiveLoop, this);

    {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait_for(lk, std::chrono::milliseconds(kSlowMs), [this] { return m_hello || m_lost; });
        if (!m_hello)
        {
            lk.unlock();
            shutdownProcess();
            throw std::runtime_error("the camera process did not start" + exitDescription());
        }
    }

    Message req = newRequest(Op::Open);
    req.putStr(vendor).putStr(connectionType).putStr(serial).putI64(index)
       .putU32(static_cast<uint32_t>(logLevel())).putU32(static_cast<uint32_t>(options.size()));
    for (const auto& kv : options)
    {
        req.putStr(kv.first).putStr(kv.second);
    }
    Reply r;
    const bool ok = call(req, r, kSlowMs);
    std::string v, m, s, c;
    int64_t idx = -1;
    if (ok && r.msg.getStr(v) && r.msg.getStr(m) && r.msg.getStr(s) && r.msg.getStr(c) && r.msg.getI64(idx))
    {
        std::lock_guard<std::mutex> lk(m_mtx);   // markLost() on the receiver may read them
        m_vendor         = v;
        m_model          = m;
        m_serial         = s;
        m_connectionType = c;
        m_index          = static_cast<int>(idx);
        DBG("RemoteCamera: " + m_vendor + " " + m_model + " " + m_serial + " runs in process "
            + std::to_string(m_pid));
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_closing = true;   // the end of the process is expected from here on
    }
    shutdownProcess();
    if (r.error != ipc::ErrorKind::None)
    {
        rethrow(r.error, r.text);
    }
    throw std::runtime_error(ok ? "the camera process sent a malformed answer to open"
                                : "the camera process ended while opening the camera" + exitDescription());
}

RemoteCamera::~RemoteCamera()
{
    close();
}

void RemoteCamera::spawn(const std::string& workerPath)
{
    if (::access(workerPath.c_str(), X_OK) != 0)
    {
        throw std::runtime_error("cannot start the camera process: '" + workerPath + "' is not an executable ("
                                 + std::strerror(errno) + ")");
    }
    int sv[2];
    // SOCK_CLOEXEC on both ends: a later camera process must not inherit this
    // camera's socket, or this one would never see end-of-file when the
    // application dies (the other process would still hold the end open).
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
    {
        throw std::runtime_error("cannot create the camera socket: " + std::string(std::strerror(errno)));
    }
    int childEnd = sv[1];
    if (childEnd == 3)
    {
        // dup2(3, 3) would not clear close-on-exec; move it out of the way.
        childEnd = ::fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
        ::close(sv[1]);
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t          attr;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, childEnd, 3);   // the only descriptor passed on
    ::posix_spawnattr_init(&attr);
    // A clean signal state, whatever the calling thread of the application had
    // blocked or ignored; the camera process installs its own handlers.
    sigset_t none, all;
    ::sigemptyset(&none);
    ::sigfillset(&all);
    ::sigdelset(&all, SIGKILL);
    ::sigdelset(&all, SIGSTOP);
    ::posix_spawnattr_setsigmask(&attr, &none);
    ::posix_spawnattr_setsigdefault(&attr, &all);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::string arg0 = workerPath;
    char fdArg[] = "--fd";
    char fdNum[] = "3";
    char* argv[] = { &arg0[0], fdArg, fdNum, nullptr };
    // posix_spawn, not fork(): the application has threads, and only the
    // calling one would survive a fork, possibly holding a lock for ever.
    const int rc = ::posix_spawn(&m_pid, workerPath.c_str(), &actions, &attr, argv, environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attr);
    ::close(childEnd);
    if (rc != 0)
    {
        ::close(sv[0]);
        m_pid = -1;
        throw std::runtime_error("cannot start the camera process '" + workerPath + "': " + std::strerror(rc));
    }
    m_sock = sv[0];
}

void RemoteCamera::close()
{
    std::lock_guard<std::mutex> cl(m_closeMtx);
    if (m_closed)
    {
        return;
    }
    ScopedLogSource tag(m_source);
    bool lost = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        lost = m_lost;
    }
    if (!lost)
    {
        Message req = newRequest(Op::Close);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closing = true;   // its end-of-file is expected now
        }
        Reply r;
        callLogged(req, r, kSlowMs);
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_closing = true;
    }
    shutdownProcess();
    m_running.store(false);
    m_closed = true;
}

void RemoteCamera::shutdownProcess()
{
    int sock = -1;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        sock = m_sock;
    }
    if (sock >= 0)
    {
        ::shutdown(sock, SHUT_RDWR);   // wakes the receiver; the camera process sees end-of-file
    }
    if (m_receiver.joinable())
    {
        m_receiver.join();
    }
    if (sock >= 0)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_sock = -1;
        }
        ::close(sock);
    }
    if (m_pid > 0 && !reap(kExitMs))
    {
        WARN("RemoteCamera: camera process " + std::to_string(m_pid) + " did not end within "
             + std::to_string(kExitMs) + " ms - killing it");
        ::kill(m_pid, SIGKILL);   // not reaped yet, so the pid is still ours
        reap(-1);
    }
}

bool RemoteCamera::reap(int timeoutMs)
{
    std::lock_guard<std::mutex> lk(m_reapMtx);
    if (m_reaped || m_pid <= 0)
    {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        int status = 0;
        const pid_t r = ::waitpid(m_pid, &status, timeoutMs < 0 ? 0 : WNOHANG);
        if (r == m_pid || (r < 0 && errno == ECHILD))
        {
            // ECHILD: somebody else in the application collected it already.
            m_reaped = true;
            m_status = (r == m_pid) ? status : 0;
            return true;
        }
        if (r < 0 && errno != EINTR)
        {
            return false;
        }
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        if (timeoutMs >= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

std::string RemoteCamera::exitDescription()
{
    reap(500);
    std::lock_guard<std::mutex> lk(m_reapMtx);
    if (!m_reaped)
    {
        return std::string();
    }
    if (WIFEXITED(m_status))
    {
        return " (exit code " + std::to_string(WEXITSTATUS(m_status)) + ")";
    }
    if (WIFSIGNALED(m_status))
    {
        const int sig = WTERMSIG(m_status);
        return " (killed by signal " + std::to_string(sig) + ", " + std::string(::strsignal(sig)) + ")";
    }
    return std::string();
}

bool RemoteCamera::isLost() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_lost;
}

void RemoteCamera::markLost(const std::string& reason)
{
    std::string who;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_lost)
        {
            return;
        }
        m_lost = true;
        if (!m_serial.empty())
        {
            who = " (" + m_vendor + " " + m_serial + ")";
        }
    }
    m_running.store(false);
    m_cv.notify_all();
    ERR("Camera process " + std::to_string(m_pid) + who + ": " + reason + exitDescription()
        + " - the camera is no longer available; images already read stay valid");
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

Message RemoteCamera::newRequest(Op op)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return Message(Kind::Request, op, m_nextId++);
}

bool RemoteCamera::call(Message& req, Reply& out, int timeoutMs)
{
    ScopedLogSource tag(m_source);
    const uint32_t id = req.id();
    int sock = -1;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // While closing only the Close request itself still goes out.
        if (m_lost || m_sock < 0 || (m_closing && req.op() != Op::Close))
        {
            return false;
        }
        sock = m_sock;
        m_waiting.insert(id);
    }
    if (!ipc::sendMessage(sock, req))
    {
        const std::string why = std::strerror(errno);
        bool closing = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_waiting.erase(id);
            closing = m_closing;
        }
        if (!closing)
        {
            markLost(std::string("cannot send the ") + opName(req.op()) + ": " + why);
        }
        return false;
    }

    std::unique_lock<std::mutex> lk(m_mtx);
    m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                  [this, id] { return m_replies.count(id) != 0 || m_lost; });
    m_waiting.erase(id);
    auto it = m_replies.find(id);
    if (it == m_replies.end())
    {
        const bool lost = m_lost;
        lk.unlock();
        if (!lost)
        {
            // A camera process that hangs keeps the device; end it so that the
            // camera can be opened again.
            {
                std::lock_guard<std::mutex> rl(m_reapMtx);
                if (!m_reaped && m_pid > 0)
                {
                    ::kill(m_pid, SIGKILL);
                }
            }
            markLost(std::string("no answer to the ") + opName(req.op()) + " within "
                     + std::to_string(timeoutMs) + " ms - process killed");
        }
        return false;
    }
    out.msg = std::move(it->second);
    m_replies.erase(it);
    lk.unlock();

    uint32_t err = 0;
    if (!out.msg.getU32(err))
    {
        WARN(std::string("RemoteCamera: malformed answer to the ") + opName(req.op()));
        return false;
    }
    if (err != static_cast<uint32_t>(ipc::ErrorKind::None))
    {
        out.error = (err <= static_cast<uint32_t>(ipc::ErrorKind::Runtime))
                        ? static_cast<ipc::ErrorKind>(err) : ipc::ErrorKind::Runtime;
        if (!out.msg.getStr(out.text))
        {
            out.text = std::string("the camera process reported an error for the ") + opName(req.op());
        }
        return false;
    }
    bool running = false;
    bool restarted = false;
    if (!out.msg.getFlag(running) || !out.msg.getFlag(restarted))
    {
        WARN(std::string("RemoteCamera: malformed answer to the ") + opName(req.op()));
        return false;
    }
    m_running.store(running);
    m_lastRestarted.store(restarted);
    return true;
}

bool RemoteCamera::callLogged(Message& req, Reply& out, int timeoutMs)
{
    if (call(req, out, timeoutMs))
    {
        return true;
    }
    if (out.error != ipc::ErrorKind::None)
    {
        ScopedLogSource tag(m_source);
        ERR("Camera process: " + out.text);
    }
    return false;
}

bool RemoteCamera::callFlag(Message req, int timeoutMs)
{
    Reply r;
    bool v = false;
    return callLogged(req, r, timeoutMs) && r.msg.getFlag(v) && v;
}

bool RemoteCamera::callValue(ipc::Quantity q, double& v)
{
    Message req = newRequest(Op::GetValue);
    req.putU32(static_cast<uint32_t>(q));
    Reply r;
    bool ok = false;
    double got = 0.0;
    if (!callLogged(req, r, kFastMs) || !r.msg.getFlag(ok) || !r.msg.getF64(got) || !ok)
    {
        return false;
    }
    v = got;
    return true;
}

bool RemoteCamera::callSize(Op op, int64_t& w, int64_t& h)
{
    Message req = newRequest(op);
    Reply r;
    bool ok = false;
    int64_t gw = 0, gh = 0;
    if (!callLogged(req, r, kFastMs) || !r.msg.getFlag(ok) || !r.msg.getI64(gw) || !r.msg.getI64(gh) || !ok)
    {
        return false;
    }
    w = gw;
    h = gh;
    return true;
}

ValueRange RemoteCamera::callRange(ipc::Quantity q)
{
    Message req = newRequest(Op::Range);
    req.putU32(static_cast<uint32_t>(q));
    Reply r;
    ValueRange out;
    bool valid = false;
    double lo = 0.0, hi = 0.0;
    if (callLogged(req, r, kFastMs) && r.msg.getFlag(valid) && r.msg.getF64(lo) && r.msg.getF64(hi))
    {
        out.valid = valid;
        out.min   = lo;
        out.max   = hi;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The CameraBase calls
// ---------------------------------------------------------------------------

namespace {

Message& withCapability(Message&& m, ipc::Capability c)
{
    return m.putU32(static_cast<uint32_t>(c));
}

} // namespace

bool RemoteCamera::canSetExposure()  { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::Exposure), kFastMs); }
bool RemoteCamera::canSetGain()      { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::Gain), kFastMs); }
bool RemoteCamera::canSetFrameRate() { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::FrameRate), kFastMs); }
bool RemoteCamera::canSetBinning()   { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::Binning), kFastMs); }
bool RemoteCamera::canSetTrigger()   { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::Trigger), kFastMs); }
bool RemoteCamera::canSetRoi()       { return callFlag(withCapability(newRequest(Op::CanSet), ipc::Capability::Roi), kFastMs); }

ValueRange RemoteCamera::exposureRangeMs() { return callRange(ipc::Quantity::ExposureMs); }
ValueRange RemoteCamera::frameRateRange()  { return callRange(ipc::Quantity::FrameRate); }
ValueRange RemoteCamera::gainRangeDb()     { return callRange(ipc::Quantity::GainDb); }

std::vector<uint32_t> RemoteCamera::binningValues()
{
    Message req = newRequest(Op::BinningValues);
    Reply r;
    uint32_t n = 0;
    if (!callLogged(req, r, kFastMs) || !r.msg.getU32(n) || n > 64)
    {
        return { 1 };
    }
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t b = 0;
        if (!r.msg.getU32(b))
        {
            return { 1 };
        }
        out.push_back(b);
    }
    return out.empty() ? std::vector<uint32_t>{ 1 } : out;
}

bool RemoteCamera::getExposureMs(double& ms) { return callValue(ipc::Quantity::ExposureMs, ms); }
bool RemoteCamera::getFrameRate(double& fps) { return callValue(ipc::Quantity::FrameRate, fps); }
bool RemoteCamera::getGainDb(double& db)     { return callValue(ipc::Quantity::GainDb, db); }

bool RemoteCamera::getBinning(uint32_t& factor)
{
    Message req = newRequest(Op::GetBinning);
    Reply r;
    bool ok = false;
    uint32_t v = 0;
    if (!callLogged(req, r, kFastMs) || !r.msg.getFlag(ok) || !r.msg.getU32(v) || !ok)
    {
        return false;
    }
    factor = v;
    return true;
}

bool RemoteCamera::getRoi(SensorRoi& roi)
{
    Message req = newRequest(Op::GetRoi);
    Reply r;
    bool ok = false;
    SensorRoi v;
    if (!callLogged(req, r, kFastMs) || !r.msg.getFlag(ok) || !r.msg.getI64(v.x) || !r.msg.getI64(v.y)
        || !r.msg.getI64(v.width) || !r.msg.getI64(v.height) || !ok)
    {
        return false;
    }
    roi = v;
    return true;
}

bool RemoteCamera::getSensorSize(int64_t& width, int64_t& height) { return callSize(Op::GetSensorSize, width, height); }
bool RemoteCamera::getImageSize(int64_t& width, int64_t& height)  { return callSize(Op::GetImageSize, width, height); }

int64_t RemoteCamera::pixelFormat()
{
    Message req = newRequest(Op::PixelFormat);
    Reply r;
    int64_t pf = 0;
    return (callLogged(req, r, kFastMs) && r.msg.getI64(pf)) ? pf : 0;
}

namespace {

Message& withValue(Message&& m, ipc::Quantity q, double v)
{
    return m.putU32(static_cast<uint32_t>(q)).putF64(v);
}

Message& withQuantity(Message&& m, ipc::Quantity q)
{
    return m.putU32(static_cast<uint32_t>(q));
}

} // namespace

bool RemoteCamera::setExposureMs(double ms) { return callFlag(withValue(newRequest(Op::SetValue), ipc::Quantity::ExposureMs, ms), kFastMs); }
bool RemoteCamera::setFrameRate(double fps) { return callFlag(withValue(newRequest(Op::SetValue), ipc::Quantity::FrameRate, fps), kFastMs); }
bool RemoteCamera::setGainDb(double db)     { return callFlag(withValue(newRequest(Op::SetValue), ipc::Quantity::GainDb, db), kFastMs); }
bool RemoteCamera::setExposureAuto()        { return callFlag(withQuantity(newRequest(Op::SetAuto), ipc::Quantity::ExposureMs), kFastMs); }
bool RemoteCamera::setGainAuto()            { return callFlag(withQuantity(newRequest(Op::SetAuto), ipc::Quantity::GainDb), kFastMs); }
bool RemoteCamera::disableFrameRateLimit()  { return callFlag(newRequest(Op::DisableFrameRateLimit), kFastMs); }

bool RemoteCamera::setBinning(uint32_t factor)
{
    Message req = newRequest(Op::SetBinning);
    req.putU32(factor);
    return callFlag(req, kSlowMs);
}

bool RemoteCamera::setRoi(const SensorRoi& roi)
{
    Message req = newRequest(Op::SetRoi);
    req.putI64(roi.x).putI64(roi.y).putI64(roi.width).putI64(roi.height);
    return callFlag(req, kSlowMs);
}

bool RemoteCamera::resetRoi() { return callFlag(newRequest(Op::ResetRoi), kSlowMs); }

void RemoteCamera::setBufferConfig(const CameraBase::BufferConfig& cfg)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cfg = cfg;
    }
    Message req = newRequest(Op::SetBufferConfig);
    req.putU32(cfg.slotCount).putStr(cfg.storagePath).putStr(cfg.comment);
    Reply r;
    callLogged(req, r, kFastMs);
}

CameraBase::BufferConfig RemoteCamera::bufferConfig() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_cfg;
}

bool RemoteCamera::setStoragePath(const std::string& dirPath)
{
    Message req = newRequest(Op::SetStoragePath);
    req.putStr(dirPath);
    const bool ok = callFlag(req, kSlowMs);
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg.storagePath = ok ? dirPath : std::string();
    return ok;
}

bool RemoteCamera::start() { return callFlag(newRequest(Op::Start), kSlowMs); }

void RemoteCamera::stop()
{
    Message req = newRequest(Op::Stop);
    Reply r;
    callLogged(req, r, kSlowMs);
}

std::string RemoteCamera::recordingFile()
{
    Message req = newRequest(Op::RecordingFile);
    Reply r;
    std::string path;
    return (callLogged(req, r, kFastMs) && r.msg.getStr(path)) ? path : std::string();
}

uint64_t RemoteCamera::framesRejected()
{
    Message req = newRequest(Op::FramesRejected);
    Reply r;
    int64_t n = 0;
    return (callLogged(req, r, kFastMs) && r.msg.getI64(n) && n > 0) ? static_cast<uint64_t>(n) : 0;
}

void RemoteCamera::streamStats(uint64_t& saved, uint64_t& dropped, bool& error)
{
    saved   = 0;
    dropped = 0;
    error   = false;
    Message req = newRequest(Op::StreamStats);
    Reply r;
    int64_t s = 0, d = 0;
    bool e = false;
    if (callLogged(req, r, kFastMs) && r.msg.getI64(s) && r.msg.getI64(d) && r.msg.getFlag(e))
    {
        saved   = s > 0 ? static_cast<uint64_t>(s) : 0;
        dropped = d > 0 ? static_cast<uint64_t>(d) : 0;
        error   = e;
    }
}

bool RemoteCamera::spotPositionFor(uint64_t sequence, double& x, double& y)
{
    Message req = newRequest(Op::SimSpotPosition);
    req.putI64(static_cast<int64_t>(sequence));
    Reply r;
    if (call(req, r, kFastMs))
    {
        return r.msg.getF64(x) && r.msg.getF64(y);
    }
    if (r.error != ipc::ErrorKind::None)
    {
        rethrow(r.error, r.text);
    }
    return false;
}

bool RemoteCamera::exposureAuto() { return callFlag(newRequest(Op::SimExposureAuto), kFastMs); }
bool RemoteCamera::gainAuto()     { return callFlag(newRequest(Op::SimGainAuto), kFastMs); }

// ---------------------------------------------------------------------------
// Reading -- straight from the attached ring, no round trip
// ---------------------------------------------------------------------------

std::shared_ptr<RingBuffer> RemoteCamera::ring() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_ring;
}

bool RemoteCamera::readLatest(CameraBase::Image& out)
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r || !r->readLatest(out.handle))
    {
        return false;
    }
    out.ring   = r;
    out.header = r->sessionHeader();
    return true;
}

bool RemoteCamera::waitForFrame(int64_t afterSequence, uint32_t timeoutMs)
{
    // As in CameraBase: the local shared_ptr keeps a replaced ring alive, and
    // the caller then wakes at the latest after timeoutMs.
    std::shared_ptr<RingBuffer> r = ring();
    if (!r)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return false;
    }
    return r->waitForFrame(afterSequence, timeoutMs);
}

bool RemoteCamera::readSlot(uint32_t slot, CameraBase::Image& out)
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r || slot >= r->slotCount() || !r->readSlot(slot, out.handle))
    {
        return false;
    }
    out.ring   = r;
    out.header = r->sessionHeader();
    return true;
}

bool RemoteCamera::sessionHeader(UC2_SessionHeader& out) const
{
    std::shared_ptr<RingBuffer> r = ring();
    if (!r)
    {
        return false;
    }
    out = r->sessionHeader();
    return true;
}

// ---------------------------------------------------------------------------
// Receiver thread
// ---------------------------------------------------------------------------

void RemoteCamera::receiveLoop()
{
    ScopedLogSource tag(m_source);
    for (;;)
    {
        Message msg;
        int fd = -1;
        std::string why;
        const ipc::RecvStatus st = ipc::recvMessage(m_sock, msg, fd, -1, why);
        if (st == ipc::RecvStatus::Closed || st == ipc::RecvStatus::Error)
        {
            bool quiet = false;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                // Closed as asked, or died before it said hello (the
                // constructor reports that one itself): no error message.
                quiet = m_closing || !m_hello;
                if (quiet)
                {
                    m_lost = true;
                }
            }
            if (!quiet)
            {
                markLost(st == ipc::RecvStatus::Closed ? "the process ended" : why);
            }
            m_cv.notify_all();
            return;
        }
        if (st == ipc::RecvStatus::Malformed)
        {
            WARN("RemoteCamera: malformed message from the camera process ignored: " + why);
            continue;
        }

        if (msg.kind() == Kind::Event)
        {
            handleEvent(msg, fd);
            continue;
        }
        if (fd >= 0)
        {
            ::close(fd);   // only Ring events carry a descriptor
        }
        if (msg.kind() == Kind::Response)
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_waiting.count(msg.id()) != 0)
                {
                    m_replies[msg.id()] = std::move(msg);
                }
            }
            m_cv.notify_all();
        }
    }
}

void RemoteCamera::handleEvent(Message& msg, int fd)
{
    switch (msg.op())
    {
    case Op::Hello:
    {
        uint32_t version = 0;
        int64_t  pid     = 0;
        if (msg.getU32(version) && msg.getI64(pid))
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_hello = true;
        }
        m_cv.notify_all();
        break;
    }
    case Op::Log:
    {
        uint32_t level = 0;
        std::string text;
        if (msg.getU32(level) && msg.getStr(text))
        {
            logMessage(sanitizedLevel(level), text);   // tagged with m_source by this thread
        }
        break;
    }
    case Op::Ring:
    {
        int64_t generation = 0;
        if (fd < 0 || !msg.getI64(generation))
        {
            WARN("RemoteCamera: ring announcement without a ring ignored");
            break;
        }
        std::shared_ptr<RingBuffer> r = RingBuffer::attach(fd);   // logs why if refused
        ::close(fd);
        fd = -1;
        std::lock_guard<std::mutex> lk(m_mtx);
        // A ring that cannot be attached must not leave the previous session's
        // frames looking current.
        m_ring = r;
        break;
    }
    default:
        WARN("RemoteCamera: unknown event " + std::to_string(static_cast<int>(msg.op())) + " ignored");
        break;
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
}

} // namespace hik
