// Smoke test for the SDK lifecycle and device enumeration. Requires the MVS
// runtime library (already linked into hikcam_core) but NOT a physical camera:
// with no device connected, enumeration simply yields an empty list.
#include <catch2/catch_test_macros.hpp>

#include "CaptureApp.h"
#include "MvsCamera.h"

TEST_CASE("SDK initialises and finalises cleanly", "[sdk]")
{
    REQUIRE(hik::MvsCamera::initSdk());
    // Safe to call even when no device is present.
    hik::MvsCamera::finalizeSdk();
}

TEST_CASE("enumeration succeeds with zero or more devices", "[sdk]")
{
    REQUIRE(hik::MvsCamera::initSdk());

    hik::MvsCamera cam;
    const int n = cam.enumerateDevices();     // count, or < 0 on error
    CHECK(n >= 0);
    CHECK(static_cast<int>(cam.devices().size()) == n);

    // The free-function view used by the Python binding must agree.
    const auto devices = hik::listDevices();
    CHECK(devices.size() == cam.devices().size());

    hik::MvsCamera::finalizeSdk();
}
