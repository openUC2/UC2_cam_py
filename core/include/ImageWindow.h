#ifndef UC2HIK_IMAGEWINDOW_H
#define UC2HIK_IMAGEWINDOW_H

#include <string>
#include <vector>

namespace hik {

/// A minimal top-level window. It serves two purposes:
///
///  - a rendering target for the MVS SDK's built-in image renderer
///    (MV_CC_DisplayOneFrameEx2). The SDK opens its own connection to draw into
///    the window's XID, so for this the class only creates/maps the window and
///    pumps its events;
///  - a plain 8-bit grey-scale viewer (showGray8()) for images that do not come
///    from an open camera, e.g. frames read back from the ring buffer.
///
/// Xlib is used without XInitThreads(), so one instance must be created and
/// used by a single thread.
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

    /// Draw an 8-bit grey-scale image at the top-left corner of the window and
    /// keep a copy so it is redrawn on Expose events (see pumpEvents()).
    /// @param pixels row-major, tightly packed, width * height bytes.
    /// @return false if no window exists or the X visual is not 24/32-bit
    ///         TrueColor.
    bool showGray8(const unsigned char* pixels, unsigned int width, unsigned int height);

    /// True once the window manager's close button has been pressed.
    bool closeRequested() const { return m_closeRequested; }

    /// True if the binary was compiled with display support at all.
    static bool displaySupported();

private:
    /// Put m_pixels into the window (no-op while no image has been shown).
    void redraw();

    void*         m_display;        ///< X11 Display* (opaque here)
    unsigned long m_window;         ///< X11 Window / XID
    unsigned long m_wmDeleteAtom;   ///< WM_DELETE_WINDOW atom
    bool          m_valid;
    bool          m_closeRequested;

    std::vector<unsigned int> m_pixels;     ///< last image as 0x00RRGGBB, for redraws
    unsigned int  m_imgWidth  = 0;
    unsigned int  m_imgHeight = 0;
    bool          m_visualWarned = false;  ///< unsupported-visual error already logged
};

} // namespace hik

#endif // UC2HIK_IMAGEWINDOW_H
