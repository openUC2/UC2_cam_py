// Hardware integration test: opens the first connected camera and grabs a
// frame. Only compiled when HIK_ENABLE_HARDWARE_TESTS=ON, and SKIPs at runtime
// when no camera is actually reachable, so it never fails a machine that simply
// has no device attached.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FrameGrabber.h"
#include "MvsCamera.h"

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

TEST_CASE("FrameGrabber streams a frame from the first device", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());

    hik::FrameGrabber g;
    if (!g.open(0))
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    REQUIRE(g.start());
    CHECK(g.isRunning());

    const hik::FrameView fv = g.waitForFrame(2000);   // up to 2 s for one frame
    CHECK(fv.valid);
    if (fv.valid)
    {
        CHECK(fv.width > 0);
        CHECK(fv.height > 0);
        CHECK(fv.data != nullptr);
        CHECK(fv.nbytes > 0);
    }

    g.stop();
    CHECK_FALSE(g.isRunning());
    CHECK(g.framesReceived() > 0);

    g.close();
    hik::MvsCamera::finalizeSdk();
}

TEST_CASE("camera parameters round-trip and ROI keeps zero-copy", "[hardware]")
{
    REQUIRE(hik::MvsCamera::initSdk());

    hik::FrameGrabber g;
    if (!g.open(0))
    {
        hik::MvsCamera::finalizeSdk();
        SKIP("no HikRobot/MVS camera connected");
    }

    // Exposure: set and read back within a tolerance (camera clamps to a grid).
    double mn = 0.0, mx = 0.0;
    REQUIRE(g.getExposureTimeRange(mn, mx));
    const double wantExposure = (mn + mx) * 0.25 > 20000.0 ? 20000.0 : (mn + mx) * 0.25;
    REQUIRE(g.setExposureTime(wantExposure));
    double gotExposure = 0.0;
    REQUIRE(g.getExposureTime(gotExposure));
    CHECK(gotExposure == Catch::Approx(wantExposure).epsilon(0.1));

    // Auto-exposure round-trip.
    REQUIRE(g.setExposureAuto(hik::AutoMode::Off));
    hik::AutoMode mode = hik::AutoMode::Continuous;
    REQUIRE(g.getExposureAuto(mode));
    CHECK(mode == hik::AutoMode::Off);

    // Sensor size and ROI.
    int64_t widthMax = 0, heightMax = 0;
    REQUIRE(g.getSensorSize(widthMax, heightMax));
    CHECK(widthMax > 0);
    CHECK(heightMax > 0);

    hik::Roi roi;
    roi.width = 640; roi.height = 480; roi.offsetX = 0; roi.offsetY = 0;
    REQUIRE(g.setRoi(roi));      // not running yet: sets directly

    REQUIRE(g.start());
    const hik::FrameView fv = g.waitForFrame(2000);
    CHECK(fv.valid);
    if (fv.valid)
    {
        // The frame reflects the ROI (aligned to the camera grid) and is still
        // an SDK-owned zero-copy node.
        CHECK(fv.width <= 640);
        CHECK(fv.height <= 480);
        CHECK(fv.width > 0);
        CHECK(fv.height > 0);
        CHECK(fv.data != nullptr);
    }

    g.stop();
    g.close();
    hik::MvsCamera::finalizeSdk();
}
