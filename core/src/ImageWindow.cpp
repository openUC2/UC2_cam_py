#include "ImageWindow.h"

#include <cstdio>
#include <cstdint>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace hik {

#ifdef HAVE_X11

ImageWindow::ImageWindow(unsigned int width, unsigned int height, const std::string& title)
    : m_display(nullptr)
    , m_window(0)
    , m_wmDeleteAtom(0)
    , m_valid(false)
    , m_closeRequested(false)
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr)
    {
        std::fprintf(stderr,
                     "ImageWindow: cannot open X display (no DISPLAY / not in a graphical session).\n"
                     "             Running without a live preview window.\n");
        return;
    }

    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(
        dpy, DefaultRootWindow(dpy),
        0, 0, width, height,
        0,                          // border width
        BlackPixel(dpy, screen),    // border colour
        BlackPixel(dpy, screen));   // background colour

    XStoreName(dpy, win, title.c_str());

    // Ask to be notified about structure changes and route the WM close button
    // through the standard WM_DELETE_WINDOW protocol instead of an X error.
    XSelectInput(dpy, win, StructureNotifyMask | ExposureMask);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);

    XMapWindow(dpy, win);

    // Wait until the window is actually mapped before the SDK draws into it.
    for (;;)
    {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == MapNotify)
        {
            break;
        }
    }
    XFlush(dpy);

    m_display      = static_cast<void*>(dpy);
    m_window       = static_cast<unsigned long>(win);
    m_wmDeleteAtom = static_cast<unsigned long>(wmDelete);
    m_valid        = true;
}

ImageWindow::~ImageWindow()
{
    if (m_display != nullptr)
    {
        Display* dpy = static_cast<Display*>(m_display);
        if (m_window != 0)
        {
            XDestroyWindow(dpy, static_cast<Window>(m_window));
        }
        XCloseDisplay(dpy);
    }
}

void* ImageWindow::nativeHandle() const
{
    if (!m_valid)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_window));
}

bool ImageWindow::pumpEvents()
{
    if (!m_valid)
    {
        return true;
    }
    Display* dpy = static_cast<Display*>(m_display);
    while (XPending(dpy) > 0)
    {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == ClientMessage &&
            static_cast<unsigned long>(e.xclient.data.l[0]) == m_wmDeleteAtom)
        {
            m_closeRequested = true;
        }
    }
    return !m_closeRequested;
}

bool ImageWindow::displaySupported()
{
    return true;
}

#else // HAVE_X11 not defined -> headless stub

ImageWindow::ImageWindow(unsigned int, unsigned int, const std::string&)
    : m_display(nullptr)
    , m_window(0)
    , m_wmDeleteAtom(0)
    , m_valid(false)
    , m_closeRequested(false)
{
}

ImageWindow::~ImageWindow() = default;

void* ImageWindow::nativeHandle() const
{
    return nullptr;
}

bool ImageWindow::pumpEvents()
{
    return true;
}

bool ImageWindow::displaySupported()
{
    return false;
}

#endif // HAVE_X11

} // namespace hik
