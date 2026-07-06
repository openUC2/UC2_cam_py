# HikCamtest

A small C++ terminal test tool for **HikRobot / MVS** industrial cameras
(GigE Vision and USB3 Vision). It

- enumerates the connected cameras,
- opens one (by index, interactively or via `--index`),
- streams frames in continuous (free-run) mode,
- prints per-frame info (size, length, pixel format) to the terminal,
- shows the **live image in a simple window** (when built with X11), and
- saves a **snapshot** to disk.

It is built directly on the SDK installed at `/opt/MVS`
(`libMvCameraControl`). The live preview uses the SDK's own renderer
(`MV_CC_DisplayOneFrameEx2`), so no pixel-format conversion or OpenCV is
needed — the SDK draws Mono/Bayer/RGB frames into the window for you.

## Project layout

```
HikCamtest/
├── CMakeLists.txt        # build configuration
├── include/              # headers
│   ├── MvsCamera.h       #   camera wrapper (enumerate/open/grab/display/save)
│   ├── ImageWindow.h     #   minimal X11 preview window (opaque, X11-free API)
│   └── MvsError.h        #   SDK error-code -> text
├── src/                  # sources
│   ├── main.cpp          #   terminal application / argument handling
│   ├── MvsCamera.cpp
│   ├── ImageWindow.cpp   #   X11 code, guarded by HAVE_X11
│   └── MvsError.cpp
└── README.md
```

## Requirements

- The MVS SDK, installed at `/opt/MVS` (already present on this machine).
  If it ever needs reinstalling, the matching archive
  `MVS-4.6.3_x86_64_20260205.tar.gz` is in this folder.
- A C++14 compiler, CMake ≥ 3.12, `make`.
- **For the preview window:** the X11 development headers.
  The runtime library is already installed; only the `-devel` headers are
  missing, so install them once:

  ```bash
  sudo dnf install libX11-devel        # Fedora / Nobara
  # or: sudo apt install libx11-dev    # Debian / Ubuntu
  ```

  Without them the project still builds and runs **headless** — it grabs
  frames and writes a snapshot to disk instead of showing a window.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

The resulting binary is `build/hikcamtest`. The MVS library path is baked in
as an rpath, so you do **not** need to export `LD_LIBRARY_PATH` to run it.

At configure time CMake reports which mode it built:

- `X11 found -> building with live preview window.`
- `... building HEADLESS (no preview window).` (install `libX11-devel` and
  re-run `cmake --build build` to switch on the window)

## Usage

```
hikcamtest [options]

  -i, --index N       Open device N (skip the interactive prompt)
  -n, --frames N      Stop after N frames (default: 0 = run until Enter / window close)
  -s, --snapshot PATH Save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif)
      --no-display    Do not open a preview window (headless)
  -h, --help          Show this help
```

Examples:

```bash
# Interactive: list cameras, pick one, live preview until you press Enter
./build/hikcamtest

# Open camera 0, grab 100 frames, save the first as PNG
./build/hikcamtest --index 0 --frames 100 --snapshot capture.png

# Headless single grab (writes snapshot.png by default when no window)
./build/hikcamtest --index 0 --frames 1 --no-display
```

When no preview window is available and you did not pass `--snapshot`, the
first frame is written to `snapshot.png` so you can still inspect the image.

## Notes / troubleshooting

- **No cameras found**
  - GigE: the camera and your network card must be in the same subnet. Use
    `/opt/MVS/bin/Ip_Configurator` to check/assign the camera IP.
  - USB3: 
  1. correct camera installation
      - Camera should be shown in lsusb
      - Camera root dir is /opt/MVS and that contains /include and /lib
  2. Usb rights are set correctly
      - ls -l /dev/bus/usb/*/* | grep -i -B0 rw-rw-rw-
      - if this doesn't show anything
        - /etc/udev/rules.d/99-mvs-usb.rules
        - Add line: SUBSYSTEM=="usb", ATTRS{idVendor}=="2bdf", MODE="0666"
        - sudo udevadm control --reload-rules
        - sudo udevadm trigger
        - Unplug camera and plug again

  3. make sure the USB filesystem buffer is large enough; the SDK ships
    `sudo /opt/MVS/bin/set_usbfs_memory_size.sh` for that.
- **Verify against Hik's own tool**: `/opt/MVS/bin/MVS` is the full GUI client
  and is handy to confirm the camera is reachable independently of this tool.
- The tool sets `TriggerMode = Off` (free-run). To test hardware triggering,
  extend `MvsCamera::open()` accordingly.
