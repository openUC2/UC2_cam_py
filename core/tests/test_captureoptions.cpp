// Unit test for the CaptureOptions defaults shared by the CLI and the binding.
#include <catch2/catch_test_macros.hpp>

#include "CaptureApp.h"

TEST_CASE("CaptureOptions has the documented defaults", "[options]")
{
    hik::CaptureOptions opts;
    CHECK(opts.index == -1);        // < 0 => ask interactively
    CHECK(opts.frames == 0);        // 0 => run until Enter / window close
    CHECK(opts.snapshot.empty());   // no snapshot by default
    CHECK(opts.display == true);    // preview on when X11 is available
}

TEST_CASE("CaptureOptions fields are independently assignable", "[options]")
{
    hik::CaptureOptions opts;
    opts.index = 2;
    opts.frames = 50;
    opts.snapshot = "shot.png";
    opts.display = false;

    CHECK(opts.index == 2);
    CHECK(opts.frames == 50);
    CHECK(opts.snapshot == "shot.png");
    CHECK(opts.display == false);
}
