// Hardware integration tests: open a connected HikRobot camera and grab. Only
// compiled when HIK_ENABLE_HARDWARE_TESTS=ON, and SKIPs at runtime when no
// camera is reachable, so it never fails a machine that simply has no device.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CaptureApp.h"
#include "HikCamera.h"
#include "MvsCamera.h"

#include <chrono>
#include <thread>

namespace {

bool waitFrames(hik::CameraBase& cam, uint64_t n, int timeoutMs = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        auto r = cam.ring();
        if (r && r->framesWritten() >= n)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("open and grab a frame from the first device", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());

    hik::MvsCamera cam;
    const int n = cam.enumerateDevices();
    if (n <= 0)
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    REQUIRE(cam.open(0));
    REQUIRE(cam.startGrabbing());

    MV_FRAME_OUT frame{};
    const bool got = cam.getFrame(frame, 2000);   // up to 2 s for one frame
    CHECK(got);
    if (got)
    {
        CHECK(frame.stFrameInfo.nExtendWidth > 0);
        CHECK(frame.stFrameInfo.nExtendHeight > 0);
        cam.freeFrame(frame);
    }

    cam.stopGrabbing();
    cam.close();
    hik::MvsCamera::finalizeSdk();
}

TEST_CASE("HikCamera fills the ring buffer from the first device", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());
    if (hik::listDevices().empty())
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    {
        hik::HikCamera cam("", "", -1);
        CHECK(cam.vendor() == "HikRobot");
        CHECK_FALSE(cam.serial().empty());
        CHECK(cam.index() == 0);

        int64_t sw = 0, sh = 0;
        REQUIRE(cam.getSensorSize(sw, sh));
        CHECK(sw > 0);
        CHECK(sh > 0);
        CHECK(hik::pixelLayout(cam.pixelFormat()).supported);

        REQUIRE(cam.start());
        CHECK(waitFrames(cam, 3));
        cam.stop();
        CHECK(cam.framesRejected() == 0);

        hik::CameraBase::Image img;
        REQUIRE(cam.readLatest(img));
        CHECK(img.handle.nbytes() == cam.ring()->frameBytes());
    }
    hik::MvsCamera::finalizeSdk();
}

TEST_CASE("HikCamera opens a camera by serial number", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());
    const auto devices = hik::listDevices();
    if (devices.empty())
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    {
        hik::HikCamera cam("", devices.back().serial, -1);
        CHECK(cam.serial() == devices.back().serial);
        CHECK(cam.connectionType() == devices.back().transport);
    }
    CHECK_THROWS_AS(hik::HikCamera("", "no-such-serial", -1), std::runtime_error);
    CHECK_THROWS_AS(hik::HikCamera("Firewire", "", -1), std::invalid_argument);
    hik::MvsCamera::finalizeSdk();
}

TEST_CASE("HikCamera parameters round-trip in interface units", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());
    if (hik::listDevices().empty())
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    {
        hik::HikCamera cam("", "", 0);
        if (cam.canSetExposure())
        {
            const hik::ValueRange r = cam.exposureRangeMs();
            REQUIRE(r.valid);
            const double want = std::min(20.0, (r.min + r.max) * 0.25);   // ms
            REQUIRE(cam.setExposureMs(want));
            double got = 0.0;
            REQUIRE(cam.getExposureMs(got));
            CHECK(got == Catch::Approx(want).epsilon(0.1));
        }
        if (cam.canSetRoi())
        {
            hik::SensorRoi roi;
            roi.width = 640; roi.height = 480;
            REQUIRE(cam.setRoi(roi));
            REQUIRE(cam.start());
            CHECK(waitFrames(cam, 1));
            hik::UC2_SessionHeader hdr{};
            REQUIRE(cam.sessionHeader(hdr));
            uint32_t b = 1;
            cam.getBinning(b);
            CHECK(hdr.width * b <= 700);
            cam.stop();
            REQUIRE(cam.resetRoi());
        }
    }
    hik::MvsCamera::finalizeSdk();
}
