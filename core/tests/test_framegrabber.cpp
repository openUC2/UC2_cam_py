// No-hardware tests for FrameGrabber lifecycle. These verify the safe-degrade
// behaviour when no camera is connected; the actual grabbing path is covered by
// the [hardware] tests in test_capture_hw.cpp.
#include <catch2/catch_test_macros.hpp>

#include "CaptureApp.h"
#include "FrameGrabber.h"
#include "MvsCamera.h"

TEST_CASE("a fresh FrameGrabber is idle", "[grabber]")
{
    hik::FrameGrabber g;
    CHECK_FALSE(g.isRunning());
    CHECK(g.framesReceived() == 0);
    CHECK(g.framesDropped() == 0);
    CHECK(g.width() == 0);
    CHECK(g.height() == 0);
}

TEST_CASE("setNodeNum keeps at least three nodes", "[grabber]")
{
    hik::FrameGrabber g;
    g.setNodeNum(1);
    CHECK(g.nodeNum() >= 3u);
    g.setNodeNum(8);
    CHECK(g.nodeNum() == 8u);
}

TEST_CASE("open() fails cleanly when no camera is present", "[grabber]")
{
    REQUIRE(hik::MvsCamera::initSdk());

    hik::FrameGrabber g;
    if (hik::listDevices().empty())
    {
        CHECK_FALSE(g.open(0));   // nothing to open
        // wait_for_frame on an unstarted grabber returns an invalid frame quickly.
        const hik::FrameView fv = g.waitForFrame(10);
        CHECK_FALSE(fv.valid);
    }

    hik::MvsCamera::finalizeSdk();
}
