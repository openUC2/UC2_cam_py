// The camera process (uc2_cam_worker) and its client RemoteCamera, with a
// simulated camera. Covers the full cycle through the real program, and the
// ways the two processes can part: closed as asked, the camera process killed,
// the application gone, garbage on the socket.
#include "hikLog.h"   // before Catch2, which defines INFO/WARN as macros

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "CamIpc.h"
#include "RemoteCamera.h"
#include "SimPattern.h"
#include "UC2Version.h"
#include "test_image_helpers.h"

using hik::CameraBase;
using hik::RemoteCamera;
using hik::ipc::Kind;
using hik::ipc::Message;
using hik::ipc::Op;
using hik::ipc::RecvStatus;

namespace fs = std::filesystem;

extern char** environ;

namespace {

const char* const kWorker = UC2_CAM_WORKER_PATH;

RemoteCamera::Options smallSim()
{
    return { { "sensor_width", "400" }, { "sensor_height", "200" }, { "frame_rate", "100" } };
}

std::unique_ptr<RemoteCamera> openSim(const RemoteCamera::Options& opts = smallSim())
{
    return std::unique_ptr<RemoteCamera>(new RemoteCamera(kWorker, "Simulated", "USB3", "", -1, opts));
}

CameraBase::BufferConfig buffer(uint32_t slots, const std::string& dir = std::string())
{
    CameraBase::BufferConfig c;
    c.slotCount   = slots;
    c.storagePath = dir;
    c.comment     = "worker test";
    return c;
}

bool processExists(pid_t pid)
{
    return ::kill(pid, 0) == 0 || errno != ESRCH;
}

std::string commOf(pid_t pid)
{
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string s;
    std::getline(f, s);
    return s;
}

template <typename F>
bool eventually(F cond, int timeoutMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        if (cond())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static int counter = 0;
        path = fs::temp_directory_path()
             / ("uc2cam_worker_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// --- the raw protocol, without RemoteCamera --------------------------------

struct RawWorker
{
    pid_t pid  = -1;
    int   sock = -1;
    uint32_t nextId = 1;

    RawWorker()
    {
        int sv[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
        posix_spawn_file_actions_t fa;
        ::posix_spawn_file_actions_init(&fa);
        ::posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
        std::string arg0 = kWorker;
        char* argv[] = { &arg0[0], nullptr };
        REQUIRE(::posix_spawn(&pid, kWorker, &fa, nullptr, argv, environ) == 0);
        ::posix_spawn_file_actions_destroy(&fa);
        ::close(sv[1]);
        sock = sv[0];
        Message hello = next();
        REQUIRE(hello.kind() == Kind::Event);
        REQUIRE(hello.op() == Op::Hello);
        uint32_t protocol = 0;
        int64_t  helloPid = 0;
        REQUIRE(hello.getU32(protocol));
        REQUIRE(hello.getI64(helloPid));
        REQUIRE(hello.getStr(helloVersion));
        CHECK(protocol == hik::ipc::kProtocolVersion);
        CHECK(helloPid == pid);
    }
    ~RawWorker()
    {
        if (sock >= 0) { ::close(sock); }
        if (pid > 0)   { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    }

    Message request(Op op) { return Message(Kind::Request, op, nextId++); }

    /// Next message that is not a log line.
    Message next(int timeoutMs = 10000)
    {
        for (;;)
        {
            Message m;
            int fd = -1;
            std::string why;
            const RecvStatus st = hik::ipc::recvMessage(sock, m, fd, timeoutMs, why);
            REQUIRE(st == RecvStatus::Ok);
            if (fd >= 0) { ::close(fd); }
            if (m.kind() == Kind::Event && m.op() == Op::Log)
            {
                logs.push_back(m);
                continue;
            }
            return m;
        }
    }

    /// Send @p req and return its response.
    Message call(Message req)
    {
        REQUIRE(hik::ipc::sendMessage(sock, req));
        for (;;)
        {
            Message m = next();
            if (m.kind() == Kind::Response && m.id() == req.id())
            {
                return m;
            }
        }
    }

    void openSim()
    {
        Message req = request(Op::Open);
        req.putStr("Simulated").putStr("USB3").putStr("").putI64(-1).putU32(10).putU32(3)
           .putStr("sensor_width").putStr("400").putStr("sensor_height").putStr("200")
           .putStr("frame_rate").putStr("100");
        Message r = call(req);
        uint32_t err = 99;
        REQUIRE(r.getU32(err));
        REQUIRE(err == 0);
    }

    /// Wait for the process to end; @return its wait status, or -1 on timeout.
    int waitExit(int timeoutMs)
    {
        int status = 0;
        const bool ended = eventually([&] { return ::waitpid(pid, &status, WNOHANG) == pid; }, timeoutMs);
        if (!ended)
        {
            return -1;
        }
        pid = -1;
        return status;
    }

    std::vector<Message> logs;
    std::string          helloVersion;   ///< software version from the Hello
};

} // namespace

// ---------------------------------------------------------------------------
// RemoteCamera
// ---------------------------------------------------------------------------

TEST_CASE("a simulated camera runs in its own process", "[worker]")
{
    std::unique_ptr<RemoteCamera> cam = openSim();
    const pid_t pid = cam->pid();
    CHECK(pid > 0);
    CHECK(pid != ::getpid());
    CHECK(commOf(pid) == "uc2cam-SIM-0001");
    CHECK(cam->vendor() == "Simulated");
    CHECK(cam->model() == "UC2-SIM");
    CHECK(cam->serial() == "SIM-0001");
    CHECK(cam->connectionType() == "USB3");
    CHECK_FALSE(cam->isLost());

    CHECK(cam->canSetRoi());
    CHECK_FALSE(cam->canSetTrigger());
    CHECK(cam->exposureRangeMs().valid);
    CHECK(cam->binningValues() == std::vector<uint32_t>{ 1, 2, 4 });
    int64_t w = 0, h = 0;
    REQUIRE(cam->getImageSize(w, h));
    CHECK(w == 400);
    CHECK(h == 200);
    REQUIRE(cam->setExposureMs(5.0));
    double ms = 0;
    REQUIRE(cam->getExposureMs(ms));
    CHECK(ms == Catch::Approx(5.0));
    CHECK(cam->pixelFormat() == hik::pfnc::Mono16);
    CHECK_FALSE(cam->ring());   // nothing before the first start

    cam->setBufferConfig(buffer(8));
    REQUIRE(cam->start());
    CHECK(cam->isRunning());
    REQUIRE(cam->ring());
    CHECK(cam->ring()->isAttached());
    REQUIRE(cam->waitForFrame(2, 5000));

    CameraBase::Image first;
    REQUIRE(cam->readLatest(first));
    CHECK(first.header.width == 400);
    CHECK(first.handle.nbytes() == 400u * 200u * 2u);
    double cx = 0, cy = 0, wantX = 0, wantY = 0;
    testimg::spotCentroid(reinterpret_cast<const uint16_t*>(first.handle.data()), 400, 200, cx, cy);
    REQUIRE(cam->spotPositionFor(first.handle.sequence(), wantX, wantY));
    if (first.handle.stillValid())   // 8 slots at 100 fps: valid for 80 ms
    {
        CHECK(cx == Catch::Approx(wantX).margin(0.5));
        CHECK(cy == Catch::Approx(wantY).margin(0.5));
    }
    CHECK(cam->framesRejected() == 0);

    // A new geometry is a new session with a new shared ring; images of the
    // old one stay readable.
    const std::shared_ptr<hik::RingBuffer> oldRing = cam->ring();
    cam->stop();
    CHECK_FALSE(cam->isRunning());
    CameraBase::Image frozen;
    REQUIRE(cam->readLatest(frozen));
    REQUIRE(cam->start());
    REQUIRE(cam->setRoi(hik::SensorRoi{ 0, 0, 200, 100 }));
    CHECK(cam->lastChangeRestarted());
    CHECK(cam->ring() != oldRing);
    REQUIRE(cam->waitForFrame(0, 5000));
    CameraBase::Image small;
    REQUIRE(cam->readLatest(small));
    CHECK(small.header.width == 200);
    CHECK(small.header.height == 100);
    CHECK(frozen.header.width == 400);
    CHECK(frozen.handle.stillValid());   // the old session is frozen: nothing recycles it

    cam->close();
    CHECK_FALSE(cam->isRunning());
    CHECK_FALSE(processExists(pid));   // collected, not a zombie
    uint64_t sum = 0;                  // the old session is still mapped here
    for (uint64_t i = 0; i < frozen.handle.nbytes(); ++i)
    {
        sum += frozen.handle.data()[i];
    }
    CHECK(sum > 0);
    cam->close();   // idempotent
}

TEST_CASE("opening errors arrive as the exception types of the in-process path", "[worker]")
{
    CHECK_THROWS_AS(RemoteCamera(kWorker, "NoSuchVendor", "", "", -1, {}), std::invalid_argument);
    CHECK_THROWS_AS(RemoteCamera(kWorker, "Simulated", "", "", -1, { { "pixel_format", "Mono12" } }),
                    std::invalid_argument);
    CHECK_THROWS_AS(RemoteCamera(kWorker, "Simulated", "", "", -1, { { "colour", "red" } }),
                    std::invalid_argument);
    CHECK_THROWS_AS(RemoteCamera(kWorker, "Simulated", "", "", -1, { { "sensor_width", "-3" } }),
                    std::invalid_argument);
    CHECK_THROWS_AS(RemoteCamera(kWorker, "HikRobot", "CameraLink", "", -1, {}), std::invalid_argument);
    CHECK_THROWS_AS(RemoteCamera("/nonexistent/uc2_cam_worker", "Simulated", "", "", -1, {}),
                    std::runtime_error);
}

TEST_CASE("the camera process says hello with its software version", "[worker]")
{
    RawWorker w;
    CHECK(w.helloVersion == hik::version());
}

TEST_CASE("a camera process of another version is refused", "[worker]")
{
    TempDir dir;
    const std::string pidFile = (dir.path / "fake.pid").string();
    ::setenv("UC2_FAKE_WORKER_PIDFILE", pidFile.c_str(), 1);
    // The exception is the whole report: no "camera lost" error besides it.
    std::atomic<int> errors{0};
    hik::setLogSink([](hik::LogLevel level, const char*, void* ctx) {
        if (level == hik::LogLevel::Error)
        {
            static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
        }
    }, &errors);

    SECTION("a different version")
    {
        ::setenv("UC2_FAKE_WORKER_VERSION", "0.0.1", 1);
        try
        {
            RemoteCamera cam(UC2_CAM_FAKE_WORKER_PATH, "Simulated", "", "", -1, {});
            FAIL("a camera process of version 0.0.1 was accepted");
        }
        catch (const std::runtime_error& e)
        {
            const std::string what = e.what();
            CHECK(what.find("version 0.0.1") != std::string::npos);
            CHECK(what.find(hik::version()) != std::string::npos);
            CHECK(what.find("reinstall") != std::string::npos);
        }
    }
    SECTION("no version at all")
    {
        ::unsetenv("UC2_FAKE_WORKER_VERSION");
        CHECK_THROWS_AS(RemoteCamera(UC2_CAM_FAKE_WORKER_PATH, "Simulated", "", "", -1, {}),
                        std::runtime_error);
    }

    // The refused process has been collected, not left behind.
    std::ifstream f(pidFile);
    pid_t pid = -1;
    REQUIRE(f >> pid);
    CHECK_FALSE(processExists(pid));
    CHECK(errors.load() == 0);

    hik::setLogSink(nullptr, nullptr);
    ::unsetenv("UC2_FAKE_WORKER_VERSION");
    ::unsetenv("UC2_FAKE_WORKER_PIDFILE");
}

TEST_CASE("a killed camera process is reported and images already read stay valid", "[worker]")
{
    std::unique_ptr<RemoteCamera> cam = openSim();
    cam->setBufferConfig(buffer(4));
    REQUIRE(cam->start());
    REQUIRE(cam->waitForFrame(1, 5000));
    CameraBase::Image img;
    REQUIRE(cam->readLatest(img));

    const pid_t pid = cam->pid();
    ::kill(pid, SIGKILL);
    REQUIRE(eventually([&] { return cam->isLost(); }, 5000));

    CHECK_FALSE(cam->isRunning());
    CHECK_FALSE(cam->canSetGain());
    CHECK_FALSE(cam->start());
    double v = 0;
    CHECK_FALSE(cam->getExposureMs(v));
    CHECK(cam->recordingFile().empty());

    // The shared pages belong to this process now: reading cannot fault.
    uint64_t sum = 0;
    const uint16_t* px = reinterpret_cast<const uint16_t*>(img.handle.data());
    for (uint64_t i = 0; i < img.handle.nbytes() / 2; ++i)
    {
        sum += px[i];
    }
    CHECK(sum > 0);
    CameraBase::Image again;
    CHECK(cam->readLatest(again));   // the last session is still mapped
    CHECK_FALSE(cam->waitForFrame(static_cast<int64_t>(cam->ring()->framesWritten()), 30));

    const auto t0 = std::chrono::steady_clock::now();
    cam->close();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK_FALSE(processExists(pid));
}

TEST_CASE("the log messages of the camera process carry its source tag", "[worker]")
{
    struct Seen
    {
        std::mutex mtx;
        std::vector<std::pair<int, std::string>> lines;
    } seen;
    hik::setLogSink([](hik::LogLevel, const char* msg, void* ctx) {
        Seen* s = static_cast<Seen*>(ctx);
        std::lock_guard<std::mutex> lk(s->mtx);
        s->lines.emplace_back(hik::currentLogSource(), msg);
    }, &seen);

    {
        std::unique_ptr<RemoteCamera> cam = openSim();
        cam->setBufferConfig(buffer(4));
        REQUIRE(cam->start());   // the worker logs "Acquisition started" before it answers
        const int source = cam->logSource();
        CHECK(source > 0);
        std::lock_guard<std::mutex> lk(seen.mtx);
        bool found = false;
        for (const auto& l : seen.lines)
        {
            if (l.second.find("Acquisition started") != std::string::npos)
            {
                found = true;
                CHECK(l.first == source);
            }
        }
        CHECK(found);
    }
    hik::setLogSink(nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// The camera process on its own
// ---------------------------------------------------------------------------

TEST_CASE("the camera process ends cleanly when the application goes away", "[worker]")
{
    TempDir dir;
    RawWorker w;
    w.openSim();

    Message cfg = w.request(Op::SetBufferConfig);
    cfg.putU32(4).putStr(dir.path.string()).putStr("raw");
    w.call(cfg);
    Message start = w.call(w.request(Op::Start));
    uint32_t err = 99;
    bool running = false, restarted = false, ok = false;
    REQUIRE(start.getU32(err));
    REQUIRE(start.getFlag(running));
    REQUIRE(start.getFlag(restarted));
    REQUIRE(start.getFlag(ok));
    REQUIRE(ok);
    CHECK(running);

    // Ctrl+C in a terminal reaches the whole process group; the camera
    // process leaves closing to the application.
    ::kill(w.pid, SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Message probe = w.call(w.request(Op::FramesRejected));
    REQUIRE(probe.getU32(err));
    CHECK(err == 0);

    ::close(w.sock);   // what the kernel does when the application dies
    w.sock = -1;
    const int status = w.waitExit(5000);
    REQUIRE(status != -1);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    // The recording was completed: header plus whole frame records only.
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir.path))
    {
        if (e.path().extension() == ".uc2rb")
        {
            files.push_back(e.path());
        }
    }
    REQUIRE(files.size() == 1);
    const uint64_t size  = fs::file_size(files[0]);
    const uint64_t frame = 64 + 400ull * 200ull * 2ull;
    CHECK(size > 8448);
    CHECK((size - 8448) % frame == 0);
}

TEST_CASE("SIGTERM ends the camera process cleanly", "[worker]")
{
    RawWorker w;
    w.openSim();
    ::kill(w.pid, SIGTERM);
    const int status = w.waitExit(5000);
    REQUIRE(status != -1);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("the camera process rejects malformed requests and keeps serving", "[worker]")
{
    RawWorker w;
    const char junk[] = "not a message";
    REQUIRE(::send(w.sock, junk, sizeof(junk), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(junk)));

    // Requests before Open fail, but properly.
    Message early = w.call(w.request(Op::Start));
    uint32_t err = 0;
    REQUIRE(early.getU32(err));
    CHECK(err == static_cast<uint32_t>(hik::ipc::ErrorKind::Runtime));

    w.openSim();

    Message cut = w.request(Op::SetRoi);
    cut.putI64(0).putI64(0);   // two of four fields
    Message r = w.call(cut);
    REQUIRE(r.getU32(err));
    CHECK(err == static_cast<uint32_t>(hik::ipc::ErrorKind::Runtime));
    std::string text;
    REQUIRE(r.getStr(text));
    CHECK(text.find("malformed") != std::string::npos);

    Message unknown(Kind::Request, static_cast<Op>(9999), 500);
    r = w.call(unknown);
    REQUIRE(r.getU32(err));
    CHECK(err == static_cast<uint32_t>(hik::ipc::ErrorKind::Runtime));

    // Still alive and answering.
    r = w.call(w.request(Op::PixelFormat));
    REQUIRE(r.getU32(err));
    CHECK(err == 0);
    bool found = false;
    for (Message& l : w.logs)
    {
        uint32_t level = 0;
        std::string line;
        if (l.getU32(level) && l.getStr(line) && line.find("malformed message ignored") != std::string::npos)
        {
            found = true;
        }
    }
    CHECK(found);

    w.call(w.request(Op::Close));
    const int status = w.waitExit(5000);
    REQUIRE(status != -1);
    CHECK(WEXITSTATUS(status) == 0);
}
