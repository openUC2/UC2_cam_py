// Unit tests for the pixel-format layout helper. Pure logic, no camera or SDK
// runtime state required.
#include <catch2/catch_test_macros.hpp>

#include "FrameGrabber.h"
#include "PixelType.h"

TEST_CASE("pixelInfo maps Mono8 to a single-channel byte image", "[pixel]")
{
    int channels = -1, itemsize = -1;
    bool simple = false;
    hik::pixelInfo(PixelType_Gvsp_Mono8, channels, itemsize, simple);
    CHECK(channels == 1);
    CHECK(itemsize == 1);
    CHECK(simple);
}

TEST_CASE("pixelInfo maps RGB8/BGR8 to three-channel byte images", "[pixel]")
{
    int channels = -1, itemsize = -1;
    bool simple = false;

    hik::pixelInfo(PixelType_Gvsp_RGB8_Packed, channels, itemsize, simple);
    CHECK(channels == 3);
    CHECK(itemsize == 1);
    CHECK(simple);

    hik::pixelInfo(PixelType_Gvsp_BGR8_Packed, channels, itemsize, simple);
    CHECK(channels == 3);
    CHECK(itemsize == 1);
    CHECK(simple);
}

TEST_CASE("pixelInfo reports unknown formats as non-simple", "[pixel]")
{
    int channels = -1, itemsize = -1;
    bool simple = true;
    hik::pixelInfo(0x0BADF00D, channels, itemsize, simple);
    CHECK_FALSE(simple);   // caller should fall back to a flat byte view
}
