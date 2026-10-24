// End-to-end tests of the acquisition path CameraBase -> RingBuffer, driven by
// the simulated camera, plus a minimal fake device for the error paths. No
// camera needed; these also run under ASan and TSan (see Docs/Tooling.md).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CameraBase.h"
#include "SimCamera.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include <unistd.h>

namespace fs = std::filesystem;
using hik::CameraBase;
using hik::SimCamera;

namespace {

/// Small, fast simulated camera: 400 x 200 Mono16 at 100 fps.
std::unique_ptr<SimCamera> makeSim(uint32_t slots = 8, const std::string& storage = std::string())
{
    SimCamera::Options opt;
    opt.sensorWidth  = 400;
    opt.sensorHeight = 200;
    opt.frameRate    = 100.0;
    auto cam = std::make_unique<SimCamera>(opt);
    CameraBase::BufferConfig cfg;
    cfg.slotCount   = slots;
    cfg.storagePath = storage;
    cfg.comment     = "test";
    cam->setBufferConfig(cfg);
    return cam;
}

/// Wait until the current ring holds at least @p n committed frames.
bool waitFrames(CameraBase& cam, uint64_t n, int timeoutMs = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        auto r = cam.ring();
        if (r && r->framesWritten() >= n)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

/// Unique, empty directory below the system temp dir; removed on destruction.
struct TempDir
{
    fs::path path;
    TempDir()
    {
        static int counter = 0;
        path = fs::temp_directory_path()
             / ("uc2cam_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

/// Intensity-weighted centroid of a Mono16 image.
void centroid(const uint16_t* img, int64_t w, int64_t h, double& cx, double& cy)
{
    double sum = 0, sx = 0, sy = 0;
    for (int64_t y = 0; y < h; ++y)
    {
        for (int64_t x = 0; x < w; ++x)
        {
            const double v = img[y * w + x];
            sum += v; sx += v * x; sy += v * y;
        }
    }
    cx = sx / sum;
    cy = sy / sum;
}

/// Minimal device that delivers exactly what a test pushes.
class FakeCamera : public CameraBase
{
public:
    explicit FakeCamera(int64_t pf = hik::pfnc::Mono8)
        : CameraBase("Fake", "FAKE", "F-1", "USB3", 0), m_pf(pf) {}
    ~FakeCamera() override { close(); }

    void push(const void* data, uint64_t n) { deliverFrame(data, n); }

    bool canSetExposure() override { return false; }
    bool canSetGain() override { return false; }
    bool canSetFrameRate() override { return false; }
    bool canSetBinning() override { return false; }
    bool canSetTrigger() override { return false; }
    bool canSetRoi() override { return false; }
    hik::ValueRange exposureRangeMs() override { return {}; }
    hik::ValueRange frameRateRange() override { return {}; }
    hik::ValueRange gainRangeDb() override { return {}; }
    std::vector<uint32_t> binningValues() override { return { 1 }; }
    bool getExposureMs(double&) override { return false; }
    bool getFrameRate(double&) override { return false; }
    bool getGainDb(double&) override { return false; }
    bool getBinning(uint32_t& b) override { b = 1; return true; }
    bool getRoi(hik::SensorRoi& r) override { r = {}; r.width = 8; r.height = 4; return true; }
    bool getSensorSize(int64_t& w, int64_t& h) override { w = 8; h = 4; return true; }
    bool getImageSize(int64_t& w, int64_t& h) override { w = 8; h = 4; return true; }
    int64_t pixelFormat() override { return m_pf; }
    bool setExposureMs(double) override { return false; }
    bool setExposureAuto() override { return false; }
    bool setFrameRate(double) override { return false; }
    bool disableFrameRateLimit() override { return false; }
    bool setGainDb(double) override { return false; }
    bool setGainAuto() override { return false; }

protected:
    bool doSetBinning(uint32_t) override { return false; }
    bool doSetRoi(const hik::SensorRoi&) override { return false; }
    bool doResetRoi() override { return false; }
    bool doStartStream() override { return true; }
    void doStopStream() override {}
    void doClose() override {}

private:
    int64_t m_pf;
};

} // namespace

TEST_CASE("nothing can be read before the first start", "[camera][sim]")
{
    auto cam = makeSim();
    CameraBase::Image img;
    CHECK_FALSE(cam->readLatest(img));
    CHECK_FALSE(cam->readSlot(0, img));
    hik::UC2_SessionHeader hdr{};
    CHECK_FALSE(cam->sessionHeader(hdr));
    CHECK(cam->recordingFile().empty());
    CHECK_FALSE(cam->isRunning());
}

TEST_CASE("a SimCamera fills the ring while running and stops cleanly", "[camera][sim]")
{
    auto cam = makeSim();
    CHECK(cam->vendor() == "Simulated");
    CHECK(cam->serial() == "SIM-0001");
    CHECK(cam->connectionType() == "USB3");

    REQUIRE(cam->start());
    CHECK(cam->isRunning());
    REQUIRE(waitFrames(*cam, 5));
    cam->stop();
    CHECK_FALSE(cam->isRunning());

    const uint64_t written = cam->ring()->framesWritten();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(cam->ring()->framesWritten() == written);   // no producer after stop()
    CHECK(cam->framesRejected() == 0);

    hik::UC2_SessionHeader hdr{};
    REQUIRE(cam->sessionHeader(hdr));
    CHECK(hdr.width == 400);
    CHECK(hdr.height == 200);
    CHECK(hdr.channels == 1);
    CHECK(hdr.depth == static_cast<uint8_t>(hik::PixelDepth::UInt16));
    CHECK(hdr.slotCount == 8);
    CHECK(std::string(hdr.cameraId) == "UC2-SIM-SIM-0001");
    CHECK(std::string(hdr.comment) == "test");
}

TEST_CASE("readLatest returns the newest frame with its spot in place", "[camera][sim]")
{
    auto cam = makeSim();
    REQUIRE(cam->start());
    REQUIRE(waitFrames(*cam, 3));
    cam->stop();   // freeze the ring so the frame cannot be recycled underneath

    CameraBase::Image img;
    REQUIRE(cam->readLatest(img));
    CHECK(img.handle.sequence() == cam->ring()->framesWritten() - 1);
    CHECK(img.handle.tsBeforeNs() > 0);
    CHECK(img.handle.tsBeforeNs() <= img.handle.tsAfterNs());
    CHECK(img.handle.nbytes() == 400u * 200u * 2u);
    CHECK(img.header.width == 400);

    double cx = 0, cy = 0;
    centroid(reinterpret_cast<const uint16_t*>(img.handle.data()), 400, 200, cx, cy);
    const hik::sim::SpotPosition want = cam->spotPositionFor(img.handle.sequence());
    CHECK(cx == Catch::Approx(want.x).margin(0.5));
    CHECK(cy == Catch::Approx(want.y).margin(0.5));
    CHECK(img.handle.stillValid());
}

TEST_CASE("waitForFrame signals new frames of a running SimCamera", "[camera][sim]")
{
    auto cam = makeSim();
    CHECK_FALSE(cam->waitForFrame(-1, 10));   // no ring before the first start

    REQUIRE(cam->start());
    REQUIRE(cam->waitForFrame(-1, 2000));
    CameraBase::Image img;
    REQUIRE(cam->readLatest(img));
    CHECK(cam->waitForFrame(static_cast<int64_t>(img.handle.sequence()), 2000));   // 100 fps: the next one
    cam->stop();

    const int64_t last = static_cast<int64_t>(cam->ring()->framesWritten()) - 1;
    CHECK_FALSE(cam->waitForFrame(last, 30));   // stopped: nothing newer arrives
}

TEST_CASE("readSlot returns frames by slot and rejects bad indices", "[camera][sim]")
{
    auto cam = makeSim(4);
    REQUIRE(cam->start());
    REQUIRE(waitFrames(*cam, 6));
    cam->stop();

    for (uint32_t s = 0; s < 4; ++s)
    {
        CameraBase::Image img;
        REQUIRE(cam->readSlot(s, img));
        CHECK(img.handle.slot() == s);
        CHECK(img.handle.sequence() % 4 == s);
    }
    CameraBase::Image bad;
    CHECK_FALSE(cam->readSlot(4, bad));
    CHECK_FALSE(cam->readSlot(1000, bad));
}

TEST_CASE("settings are clamped to the simulated ranges", "[camera][sim]")
{
    auto cam = makeSim();
    double v = 0.0;

    REQUIRE(cam->setExposureMs(1e9));
    REQUIRE(cam->getExposureMs(v));
    CHECK(v == Catch::Approx(cam->exposureRangeMs().max));
    REQUIRE(cam->setExposureAuto());
    CHECK(cam->exposureAuto());
    REQUIRE(cam->setExposureMs(5.0));
    CHECK_FALSE(cam->exposureAuto());   // a manual value switches auto off

    REQUIRE(cam->setFrameRate(0.0));
    REQUIRE(cam->getFrameRate(v));
    CHECK(v == Catch::Approx(cam->frameRateRange().min));

    REQUIRE(cam->setGainDb(-5.0));
    REQUIRE(cam->getGainDb(v));
    CHECK(v == Catch::Approx(0.0));

    REQUIRE(cam->setBinning(3));
    uint32_t b = 0;
    REQUIRE(cam->getBinning(b));
    CHECK(b == 2);
}

TEST_CASE("ROI and binning change the image geometry while stopped", "[camera][sim]")
{
    auto cam = makeSim();
    REQUIRE(cam->setBinning(2));
    int64_t w = 0, h = 0;
    REQUIRE(cam->getImageSize(w, h));
    CHECK(w == 200);
    CHECK(h == 100);
    CHECK_FALSE(cam->lastChangeRestarted());

    hik::SensorRoi roi;
    roi.x = 33; roi.y = 21; roi.width = 203; roi.height = 99;
    REQUIRE(cam->setRoi(roi));
    hik::SensorRoi got;
    REQUIRE(cam->getRoi(got));
    // Snapped to the simulated grid (binned: width step 8, height step 2,
    // offsets step 2) and expressed again in sensor pixels.
    CHECK(got.width % 16 == 0);
    CHECK(got.height % 4 == 0);
    CHECK(got.x % 4 == 0);
    CHECK(got.width == Catch::Approx(203).margin(16));

    REQUIRE(cam->start());
    REQUIRE(waitFrames(*cam, 1));
    hik::UC2_SessionHeader hdr{};
    REQUIRE(cam->sessionHeader(hdr));
    CHECK(hdr.width == got.width / 2);
    CHECK(hdr.height == got.height / 2);

    REQUIRE(cam->resetRoi());   // while running -> restart
    CHECK(cam->lastChangeRestarted());
    REQUIRE(cam->getRoi(got));
    CHECK(got.width == 400);
    CHECK(got.height == 200);
    cam->stop();
}

TEST_CASE("changing the ROI while recording starts a new session file", "[camera][sim][stream]")
{
    TempDir dir;
    auto cam = makeSim(8, dir.path.string());
    REQUIRE(cam->start());
    REQUIRE(waitFrames(*cam, 3));
    const std::string first = cam->recordingFile();
    CHECK_FALSE(first.empty());

    hik::SensorRoi roi;
    roi.width = 200; roi.height = 100;
    REQUIRE(cam->setRoi(roi));
    CHECK(cam->lastChangeRestarted());
    CHECK(cam->isRunning());
    const std::string second = cam->recordingFile();
    CHECK(second != first);
    REQUIRE(waitFrames(*cam, 2));
    cam->stop();

    CHECK(fs::exists(first));
    CHECK(fs::exists(second));
}

TEST_CASE("recording writes the header plus every saved frame", "[camera][sim][stream]")
{
    TempDir dir;
    auto cam = makeSim(8, dir.path.string());
    REQUIRE(cam->start());
    REQUIRE(waitFrames(*cam, 10));
    cam->stop();

    const auto ring = cam->ring();
    const std::string path = cam->recordingFile();
    REQUIRE(fs::exists(path));
    CHECK(ring->streamFramesSaved() > 0);
    CHECK_FALSE(ring->streamHasError());
    CHECK(fs::file_size(path)
          == sizeof(hik::UC2_SessionHeader)
             + ring->streamFramesSaved() * (sizeof(hik::UC2_FrameHeader) + ring->frameBytes()));
    CHECK(fs::exists(fs::path(path).replace_extension(".json")));
}

TEST_CASE("storing can be switched during a run without a restart", "[camera][sim][stream]")
{
    TempDir dir;
    auto cam = makeSim();                     // starts without a storage path
    REQUIRE(cam->start());
    const auto ring = cam->ring();
    REQUIRE(waitFrames(*cam, 3));
    CHECK(cam->recordingFile().empty());

    REQUIRE(cam->setStoragePath(dir.path.string()));
    const std::string path = cam->recordingFile();
    CHECK_FALSE(path.empty());
    REQUIRE(waitFrames(*cam, ring->framesWritten() + 5));
    REQUIRE(cam->setStoragePath(""));

    CHECK(cam->ring() == ring);               // the same session went on
    CHECK(cam->isRunning());
    CHECK(cam->bufferConfig().storagePath.empty());
    CHECK(ring->streamFramesSaved() >= 5);
    CHECK(fs::file_size(path)
          == sizeof(hik::UC2_SessionHeader)
             + ring->streamFramesSaved() * (sizeof(hik::UC2_FrameHeader) + ring->frameBytes()));

    const uint64_t size = fs::file_size(path);
    REQUIRE(waitFrames(*cam, ring->framesWritten() + 3));
    CHECK(fs::file_size(path) == size);       // switched off: the file stays as it was
    cam->stop();
}

TEST_CASE("a storage path that cannot be created switches storing off", "[camera][sim][stream]")
{
    auto cam = makeSim();
    REQUIRE(cam->start());
    CHECK_FALSE(cam->setStoragePath("/dev/null/this-cannot-be-a-directory"));
    CHECK(cam->bufferConfig().storagePath.empty());
    CHECK(cam->isRunning());                  // the acquisition is not affected
    CHECK_FALSE(cam->ring()->isStreaming());
    cam->stop();
}

TEST_CASE("an image keeps its ring alive after the camera is gone", "[camera][sim]")
{
    CameraBase::Image img;
    {
        auto cam = makeSim();
        REQUIRE(cam->start());
        REQUIRE(waitFrames(*cam, 2));
        REQUIRE(cam->readLatest(img));
        cam->close();
        CHECK_FALSE(cam->start());   // closed for good
    }   // camera destroyed here
    REQUIRE(img.ring);
    const uint16_t* px = reinterpret_cast<const uint16_t*>(img.handle.data());
    uint64_t sum = 0;
    for (uint64_t i = 0; i < img.handle.nbytes() / 2; ++i)
    {
        sum += px[i];   // under ASan: no use-after-free
    }
    CHECK(sum > 0);
}

TEST_CASE("frames of the wrong size are rejected and counted", "[camera]")
{
    FakeCamera cam;
    REQUIRE(cam.start());
    uint8_t good[32] = {};
    uint8_t bad[10]  = {};
    cam.push(good, sizeof(good));
    cam.push(bad, sizeof(bad));
    cam.push(good, sizeof(good));
    CHECK(cam.ring()->framesWritten() == 2);
    CHECK(cam.framesRejected() == 1);
    cam.stop();

    // After stop() pushed frames are ignored, not stored.
    cam.push(good, sizeof(good));
    CHECK(cam.ring()->framesWritten() == 2);
}

TEST_CASE("start fails cleanly for a pixel format that cannot be stored", "[camera]")
{
    FakeCamera cam(hik::pfnc::Mono12Packed);
    CHECK_FALSE(cam.start());
    CHECK_FALSE(cam.isRunning());
    CHECK(cam.ring() == nullptr);
}

TEST_CASE("start fails cleanly for an invalid slot count", "[camera]")
{
    FakeCamera cam;
    CameraBase::BufferConfig cfg;
    cfg.slotCount = 1;   // the ring needs at least 2
    cam.setBufferConfig(cfg);
    CHECK_FALSE(cam.start());
    CHECK_FALSE(cam.isRunning());
}
