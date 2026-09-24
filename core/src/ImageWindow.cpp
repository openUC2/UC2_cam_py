#include "ImageWindow.h"
#include "hikLog.h"

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
        ERR("ImageWindow: cannot open X display (no DISPLAY / not in a graphical session). Running without a live preview window.");
        
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
        else if (e.type == Expose && e.xexpose.count == 0)
        {
            redraw();
        }
    }
    return !m_closeRequested;
}

bool ImageWindow::showGray8(const unsigned char* pixels, unsigned int width, unsigned int height)
{
    if (!m_valid || pixels == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    // Only the common 24/32-bit TrueColor layout (0x00RRGGBB) is handled; that
    // is what every current X server and XWayland hand out by default.
    Display* dpy    = static_cast<Display*>(m_display);
    const int scr   = DefaultScreen(dpy);
    Visual*  visual = DefaultVisual(dpy, scr);
    const int depth = DefaultDepth(dpy, scr);
    if (depth < 24 || visual->red_mask != 0xFF0000ul || visual->green_mask != 0x00FF00ul
        || visual->blue_mask != 0x0000FFul)
    {
        if (!m_visualWarned)
        {
            ERR("ImageWindow::showGray8: unsupported X visual (depth " + std::to_string(depth)
                + "), only 24/32-bit TrueColor is handled");
            m_visualWarned = true;
        }
        return false;
    }

    const size_t n = static_cast<size_t>(width) * height;
    m_pixels.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned int g = pixels[i];
        m_pixels[i] = (g << 16) | (g << 8) | g;
    }
    m_imgWidth  = width;
    m_imgHeight = height;

    redraw();
    return true;
}

void ImageWindow::redraw()
{
    if (!m_valid || m_pixels.empty())
    {
        return;
    }
    Display* dpy  = static_cast<Display*>(m_display);
    const int scr = DefaultScreen(dpy);

    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr), ZPixmap, 0,
                               reinterpret_cast<char*>(m_pixels.data()),
                               m_imgWidth, m_imgHeight, 32, 0);
    if (img == nullptr)
    {
        ERR("ImageWindow: XCreateImage failed");
        return;
    }
    // The pixels are host-order 32-bit words; tell Xlib so it swaps if the
    // server's byte order differs.
    const uint32_t probe = 1;
    img->byte_order = (*reinterpret_cast<const unsigned char*>(&probe) == 1) ? LSBFirst : MSBFirst;

    XPutImage(dpy, static_cast<Window>(m_window), DefaultGC(dpy, scr), img,
              0, 0, 0, 0, m_imgWidth, m_imgHeight);
    XFlush(dpy);

    img->data = nullptr;   // owned by m_pixels, XDestroyImage must not free() it
    XDestroyImage(img);
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

bool ImageWindow::showGray8(const unsigned char*, unsigned int, unsigned int)
{
    return false;
}

void ImageWindow::redraw()
{
}

bool ImageWindow::displaySupported()
{
    return false;
}

#endif // HAVE_X11

} // namespace hik
