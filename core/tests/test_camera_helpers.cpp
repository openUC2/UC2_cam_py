// Unit tests for the vendor-neutral camera helpers in CameraBase.h and the
// simulated spot pattern. Pure logic, no camera or SDK state required.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CameraBase.h"
#include "PixelType.h"
#include "SimPattern.h"

#include <vector>

using hik::PixelDepth;

TEST_CASE("PFNC constants match the MVS SDK pixel types", "[camera][pixel]")
{
    CHECK(hik::pfnc::Mono8 == PixelType_Gvsp_Mono8);
    CHECK(hik::pfnc::Mono10 == PixelType_Gvsp_Mono10);
    CHECK(hik::pfnc::Mono10Packed == PixelType_Gvsp_Mono10_Packed);
    CHECK(hik::pfnc::Mono12 == PixelType_Gvsp_Mono12);
    CHECK(hik::pfnc::Mono12Packed == PixelType_Gvsp_Mono12_Packed);
    CHECK(hik::pfnc::Mono16 == PixelType_Gvsp_Mono16);
    CHECK(hik::pfnc::BayerGR8 == PixelType_Gvsp_BayerGR8);
    CHECK(hik::pfnc::BayerBG8 == PixelType_Gvsp_BayerBG8);
    CHECK(hik::pfnc::BayerGR10 == PixelType_Gvsp_BayerGR10);
    CHECK(hik::pfnc::BayerBG10 == PixelType_Gvsp_BayerBG10);
    CHECK(hik::pfnc::BayerGR12 == PixelType_Gvsp_BayerGR12);
    CHECK(hik::pfnc::BayerBG12 == PixelType_Gvsp_BayerBG12);
    CHECK(hik::pfnc::BayerGR10Packed == PixelType_Gvsp_BayerGR10_Packed);
    CHECK(hik::pfnc::BayerBG12Packed == PixelType_Gvsp_BayerBG12_Packed);
    CHECK(hik::pfnc::BayerGR16 == PixelType_Gvsp_BayerGR16);
    CHECK(hik::pfnc::BayerBG16 == PixelType_Gvsp_BayerBG16);
    CHECK(hik::pfnc::RGB8 == PixelType_Gvsp_RGB8_Packed);
    CHECK(hik::pfnc::BGR8 == PixelType_Gvsp_BGR8_Packed);
    CHECK(hik::pfnc::YUV422_8_UYVY == PixelType_Gvsp_YUV422_Packed);
    CHECK(hik::pfnc::YUV422_8 == PixelType_Gvsp_YUV422_YUYV_Packed);
}

TEST_CASE("pixelLayout maps the storable formats", "[camera][pixel]")
{
    struct Case { int64_t pf; uint32_t channels; PixelDepth depth; const char* name; };
    const std::vector<Case> cases = {
        { PixelType_Gvsp_Mono8,       1, PixelDepth::UInt8,  "Mono8" },
        { PixelType_Gvsp_Mono10,      1, PixelDepth::UInt16, "Mono10" },
        { PixelType_Gvsp_Mono12,      1, PixelDepth::UInt16, "Mono12" },
        { PixelType_Gvsp_Mono16,      1, PixelDepth::UInt16, "Mono16" },
        { PixelType_Gvsp_BayerRG8,    1, PixelDepth::UInt8,  "BayerRG8" },
        { PixelType_Gvsp_BayerGB10,   1, PixelDepth::UInt16, "BayerGB10" },
        { PixelType_Gvsp_BayerRG12,   1, PixelDepth::UInt16, "BayerRG12" },
        { PixelType_Gvsp_BayerGB16,   1, PixelDepth::UInt16, "BayerGB16" },
        { PixelType_Gvsp_RGB8_Packed, 3, PixelDepth::UInt8,  "RGB8" },
        { PixelType_Gvsp_BGR8_Packed, 3, PixelDepth::UInt8,  "BGR8" },
    };
    for (const Case& c : cases)
    {
        INFO("format " << c.name);
        const hik::PixelLayout l = hik::pixelLayout(c.pf);
        CHECK(l.supported);
        CHECK(l.channels == c.channels);
        CHECK(l.depth == c.depth);
        CHECK(std::string(l.name) == c.name);
    }
}

TEST_CASE("pixelLayout rejects packed, YUV and unknown formats", "[camera][pixel]")
{
    CHECK_FALSE(hik::pixelLayout(PixelType_Gvsp_Mono12_Packed).supported);
    CHECK_FALSE(hik::pixelLayout(PixelType_Gvsp_Mono10_Packed).supported);
    CHECK_FALSE(hik::pixelLayout(PixelType_Gvsp_BayerRG12_Packed).supported);
    CHECK_FALSE(hik::pixelLayout(PixelType_Gvsp_YUV422_Packed).supported);
    CHECK_FALSE(hik::pixelLayout(0x0BADF00D).supported);
}

TEST_CASE("unpackedAlternatives offers the matching unpacked format", "[camera][pixel]")
{
    using V = std::vector<int64_t>;
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_Mono10_Packed) == V{ PixelType_Gvsp_Mono10 });
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_Mono12_Packed) == V{ PixelType_Gvsp_Mono12 });
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_BayerGB10_Packed) == V{ PixelType_Gvsp_BayerGB10 });
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_BayerRG12_Packed) == V{ PixelType_Gvsp_BayerRG12 });
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_YUV422_Packed)
          == V{ PixelType_Gvsp_RGB8_Packed, PixelType_Gvsp_BGR8_Packed });
    CHECK(hik::unpackedAlternatives(PixelType_Gvsp_Mono8).empty());
    for (int64_t alt : hik::unpackedAlternatives(PixelType_Gvsp_BayerBG12_Packed))
    {
        CHECK(hik::pixelLayout(alt).supported);
    }
}

TEST_CASE("nearestOnGrid snaps to the closest grid point inside the range", "[camera]")
{
    CHECK(hik::nearestOnGrid(13, 0, 100, 8) == 16);    // 13 is closer to 16 than to 8
    CHECK(hik::nearestOnGrid(11, 0, 100, 8) == 8);
    CHECK(hik::nearestOnGrid(12, 0, 100, 8) == 16);    // tie goes up
    CHECK(hik::nearestOnGrid(-5, 16, 100, 8) == 16);   // below min -> min
    CHECK(hik::nearestOnGrid(1000, 16, 100, 8) == 96); // above max -> top grid point <= max
    CHECK(hik::nearestOnGrid(99, 16, 100, 8) == 96);   // would round to 104 > max
    CHECK(hik::nearestOnGrid(7, 0, 10, 0) == 7);       // inc 0 is treated as 1
    CHECK(hik::nearestOnGrid(5, 10, 3, 1) == 10);      // max < min collapses to min
}

TEST_CASE("nearestBinning picks the closest supported factor", "[camera]")
{
    const std::vector<uint32_t> s = { 1, 2, 4 };
    CHECK(hik::nearestBinning(1, s) == 1);
    CHECK(hik::nearestBinning(3, s) == 2);    // tie between 2 and 4 -> smaller
    CHECK(hik::nearestBinning(5, s) == 4);
    CHECK(hik::nearestBinning(100, s) == 4);
    CHECK(hik::nearestBinning(0, s) == 1);
    CHECK(hik::nearestBinning(1, { 2, 4 }) == 2);
    CHECK(hik::nearestBinning(3, {}) == 1);
}

TEST_CASE("clampValue keeps values inside the interval", "[camera]")
{
    CHECK(hik::clampValue(5.0, 1.0, 10.0) == 5.0);
    CHECK(hik::clampValue(-1.0, 1.0, 10.0) == 1.0);
    CHECK(hik::clampValue(11.0, 1.0, 10.0) == 10.0);
}

TEST_CASE("ROI converts between sensor and binned pixels", "[camera]")
{
    hik::SensorRoi s;
    s.x = 100; s.y = 50; s.width = 640; s.height = 480;
    const hik::SensorRoi b = hik::toBinned(s, 2);
    CHECK(b.x == 50);
    CHECK(b.y == 25);
    CHECK(b.width == 320);
    CHECK(b.height == 240);
    const hik::SensorRoi back = hik::toSensor(b, 2);
    CHECK(back.x == s.x);
    CHECK(back.width == s.width);
    CHECK(hik::toBinned(s, 0).width == 640);   // factor 0 is treated as 1
}

TEST_CASE("the simulated spot walks over a 5 x 4 grid", "[camera][sim]")
{
    const hik::sim::SpotPosition p0 = hik::sim::spotPosition(0, 1920, 1200);
    CHECK(p0.x == Catch::Approx(192.0));
    CHECK(p0.y == Catch::Approx(150.0));
    const hik::sim::SpotPosition p6 = hik::sim::spotPosition(6, 1920, 1200);
    CHECK(p6.x == Catch::Approx(576.0));
    CHECK(p6.y == Catch::Approx(450.0));
    const hik::sim::SpotPosition p20 = hik::sim::spotPosition(20, 1920, 1200);
    CHECK(p20.x == Catch::Approx(p0.x));   // wraps after 20 frames
    CHECK(p20.y == Catch::Approx(p0.y));
}

TEST_CASE("renderSpot draws the spot at the requested position", "[camera][sim]")
{
    hik::SensorRoi roi;
    roi.width = 400; roi.height = 200;
    const hik::sim::SpotPosition c{ 120.0, 80.0 };

    SECTION("Mono16, no binning")
    {
        std::vector<uint16_t> img(400 * 200);
        hik::sim::renderSpot(img.data(), PixelDepth::UInt16, roi, 1, c, 20.0, 60000.0);
        CHECK(img[80 * 400 + 120] == 60000);   // peak at the centre
        CHECK(img[0] == 0);                    // background far away

        double sum = 0, sx = 0, sy = 0;
        for (int y = 0; y < 200; ++y)
        {
            for (int x = 0; x < 400; ++x)
            {
                const double v = img[static_cast<size_t>(y) * 400 + x];
                sum += v; sx += v * x; sy += v * y;
            }
        }
        CHECK(sx / sum == Catch::Approx(120.0).margin(0.05));
        CHECK(sy / sum == Catch::Approx(80.0).margin(0.05));
    }

    SECTION("Mono8 with binning 2 halves the image")
    {
        std::vector<uint8_t> img(200 * 100);
        hik::sim::renderSpot(img.data(), PixelDepth::UInt8, roi, 2, c, 20.0, 240.0);
        // binned pixel 60/40 covers sensor pixels 120..121 / 80..81 -> just off the peak
        CHECK(img[40 * 200 + 60] > 230);
        CHECK(img[0] == 0);
    }
}
