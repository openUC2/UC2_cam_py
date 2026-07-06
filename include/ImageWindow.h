#ifndef HIKCAMTEST_IMAGEWINDOW_H
#define HIKCAMTEST_IMAGEWINDOW_H

#include <string>

namespace hik {

/// A minimal top-level window used purely as a rendering target for the MVS
/// SDK's built-in image renderer (MV_CC_DisplayOneFrameEx2). The SDK opens its
/// own connection to draw into the window's XID, so this class only has to
/// create/map the window and pump its events.
///
/// The public interface carries no X11 types, so this header compiles whether
/// or not X11 development headers are installed. When the project is built
/// without X11 (HAVE_X11 undefined) every method degrades to a safe no-op and
/// isValid() returns false, letting the application run headless.
class ImageWindow
{
public:
    ImageWindow(unsigned int width, unsigned int height, const std::string& title);
    ~ImageWindow();

    ImageWindow(const ImageWindow&) = delete;
    ImageWindow& operator=(const ImageWindow&) = delete;

    /// True if a real, mapped window exists and can be drawn into.
    bool isValid() const { return m_valid; }

    /// Native window handle for MV_CC_DisplayOneFrameEx2 (the X11 Window cast to
    /// void*), or nullptr when no window exists.
    void* nativeHandle() const;

    /// Process pending window-system events without blocking. Returns false once
    /// the user has asked to close the window.
    bool pumpEvents();

    /// True once the window manager's close button has been pressed.
    bool closeRequested() const { return m_closeRequested; }

    /// True if the binary was compiled with display support at all.
    static bool displaySupported();

private:
    void*         m_display;        ///< X11 Display* (opaque here)
    unsigned long m_window;         ///< X11 Window / XID
    unsigned long m_wmDeleteAtom;   ///< WM_DELETE_WINDOW atom
    bool          m_valid;
    bool          m_closeRequested;
};

} // namespace hik

#endif // HIKCAMTEST_IMAGEWINDOW_H
