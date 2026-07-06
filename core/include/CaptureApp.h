#ifndef HIKCAMTEST_CAPTUREAPP_H
#define HIKCAMTEST_CAPTUREAPP_H

#include <string>
#include <vector>

#include "MvsCamera.h"

namespace hik {

/// Options controlling a capture session (shared by the CLI and the Python
/// binding).
struct CaptureOptions
{
    int         index    = -1;      ///< device index; < 0 => ask interactively
    long        frames   = 0;       ///< stop after N frames; 0 => until Enter/close
    std::string snapshot;           ///< path for the first-frame snapshot (may be empty)
    bool        display  = true;    ///< open a preview window when X11 support is available
};

/// Enumerate the connected cameras and return their descriptions.
/// Precondition: the SDK has been initialised (MvsCamera::initSdk()).
std::vector<DeviceInfo> listDevices();

/// Run one capture session: enumerate, open the selected device, stream frames
/// (printing per-frame info, showing a preview window when available and saving
/// a snapshot), then clean up. Returns 0 on success, 2 if no camera was found,
/// and 1 on any other error.
/// Precondition: the SDK has been initialised (MvsCamera::initSdk()).
int runCapture(const CaptureOptions& opts);

} // namespace hik

#endif // HIKCAMTEST_CAPTUREAPP_H
