// Unit tests for the ROI clamp/align helper. Pure integer logic, no camera.
#include <catch2/catch_test_macros.hpp>

#include "MvsCamera.h"

TEST_CASE("alignRoiValue rounds down to the increment grid", "[roi]")
{
    // Grid anchored at min=0, step 16: 700 -> 688.
    CHECK(hik::alignRoiValue(700, 0, 1920, 16) == 688);
    // Exact multiples are unchanged.
    CHECK(hik::alignRoiValue(640, 0, 1920, 16) == 640);
}

TEST_CASE("alignRoiValue clamps to the range", "[roi]")
{
    CHECK(hik::alignRoiValue(-50, 0, 1920, 16) == 0);       // below min
    CHECK(hik::alignRoiValue(5000, 0, 1920, 16) == 1920);   // above max (1920 is on-grid)
}

TEST_CASE("alignRoiValue honours a non-zero minimum anchor", "[roi]")
{
    // Grid = min + k*inc = 8, 12, 16, ...; request 15 -> 12.
    CHECK(hik::alignRoiValue(15, 8, 100, 4) == 12);
}

TEST_CASE("alignRoiValue tolerates a zero/invalid increment", "[roi]")
{
    // inc < 1 is treated as 1 (no rounding), only clamping applies.
    CHECK(hik::alignRoiValue(123, 0, 1000, 0) == 123);
}
