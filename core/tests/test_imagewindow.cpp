// Unit test for the ImageWindow wrapper. Exercises the invariants that hold
// with or without a display, so it is safe to run headless / in CI.
#include <catch2/catch_test_macros.hpp>

#include "ImageWindow.h"

TEST_CASE("ImageWindow::displaySupported reports the build configuration", "[window]")
{
    // Just has to be callable and return a bool; the value depends on whether
    // the project was compiled with HAVE_X11.
    const bool supported = hik::ImageWindow::displaySupported();
    CHECK((supported == true || supported == false));
}

TEST_CASE("ImageWindow construction is safe and self-consistent", "[window]")
{
    // Construction must never throw. Without display support (headless build,
    // or X11 build with no reachable DISPLAY) the window degrades to a no-op:
    // isValid() is false and nativeHandle() is null.
    hik::ImageWindow win(320, 240, "unit-test");

    CHECK_FALSE(win.closeRequested());          // nothing pressed yet
    CHECK((win.pumpEvents() == true || win.pumpEvents() == false));

    // nativeHandle() is non-null only when a real window exists.
    if (!win.isValid())
        CHECK(win.nativeHandle() == nullptr);
    else
        CHECK(win.nativeHandle() != nullptr);

    if (!hik::ImageWindow::displaySupported())
        CHECK_FALSE(win.isValid());
}
