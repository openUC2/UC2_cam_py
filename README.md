# HikCamtest

A small test tool for **HikRobot / MVS** industrial cameras (GigE Vision and
USB3 Vision), available both as a **C++ terminal application** and as a
**Python package** (via pybind11). It

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
├── core/                     # the C++ library + standalone terminal app
│   ├── CMakeLists.txt        #   builds hikcamtest_core (lib) and hikcamtest (app)
│   ├── include/              #   headers
│   │   ├── CaptureApp.h      #     capture session shared by CLI + Python
│   │   ├── MvsCamera.h       #     camera wrapper (enumerate/open/grab/display/save)
│   │   ├── ImageWindow.h     #     minimal X11 preview window (opaque, X11-free API)
│   │   └── MvsError.h        #     SDK error-code -> text
│   └── src/                  #   sources (main.cpp is the CLI; the rest is the lib)
├── bindings/
│   └── hikcamtest_py.cpp     # pybind11 module (_hikcamtest)
├── python/hikcamtest/        # the Python package
│   ├── __init__.py           #   initialises the SDK on import
│   └── __main__.py           #   `python -m hikcamtest` CLI
├── tests/test_start.py       # short test program: start HikCamtest from Python
├── CMakeLists.txt            # top-level build (Python extension) for scikit-build-core
├── pyproject.toml            # Python packaging (scikit-build-core + pybind11, uv)
├── .python-version           # pins Python to 3.12
└── README.md
```

## Requirements

- The MVS SDK, commonly installed at `/opt/MVS`.
- A C++14 compiler, CMake ≥ 3.15, `make`/`ninja`.
- For the Python package: [uv](https://docs.astral.sh/uv/) and Python 3.12 or higher
  (uv installs it automatically from `.python-version`).
- **For the preview window:** the X11 development headers (already installed
  here). If they are ever missing, install them once:

  ```bash
  sudo dnf install libX11-devel        # Fedora / Nobara
  # or: sudo apt install libx11-dev    # Debian / Ubuntu
  ```

  Without them everything still builds and runs **headless** — it grabs frames
  and writes a snapshot to disk instead of showing a window.

## Preparing CMake
- Initially CMake might fail, due to the absence of pybind11
- pybind11 should be installed to the .venv when running

```bash
uv sync
```

- if you are using vscode, just add the following to the .vscode/settings.json (make sure the paths are correct)

```json
// .vscode/settings.json
{
  "cmake.environment": {
    "VIRTUAL_ENV": "${workspaceFolder}/.venv",
    "PATH": "${workspaceFolder}/.venv/bin:${env:PATH}"
  },
  "cmake.configureSettings": {
    "Python_EXECUTABLE": "${workspaceFolder}/.venv/bin/python",
    "pybind11_DIR": "${workspaceFolder}/.venv/lib/python3.12/site-packages/pybind11/share/cmake/pybind11"
  }
}
```

## Build & run — Python (uv)

```bash
uv sync                              # creates .venv (Python 3.12), builds the extension
uv run python tests/test_start.py    # short test program: lists cameras and starts a capture
```

From Python:

```python
import hikcamtest

for i, dev in enumerate(hikcamtest.list_devices()):
    print(i, dev)

# index / frames / snapshot / display are all optional
hikcamtest.run(index=0, frames=50, snapshot="snapshot.png", display=True)
```

`uv run python -m hikcamtest --index 0 --frames 100 --snapshot capture.png`
runs the same thing from the command line. The MVS runtime libraries (including
the renderer) are located automatically, so you do **not** need to export
`LD_LIBRARY_PATH`.

## Build & run — C++ standalone (core)

```bash
cmake -S core -B build/core
cmake --build build/core -j
./build/core/hikcamtest              # interactive: list cameras, pick one, live preview
```

At configure time CMake reports which mode it built:

- `X11 found -> building with live preview window.`
- `... building HEADLESS (no preview window).` (install `libX11-devel` and
  rebuild to switch on the window)

Options:

```
hikcamtest [options]

  -i, --index N       Open device N (skip the interactive prompt)
  -n, --frames N      Stop after N frames (default: 0 = run until Enter / window close)
  -s, --snapshot PATH Save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif)
      --no-display    Do not open a preview window (headless)
  -h, --help          Show this help
```

```bash
# Open camera 0, grab 100 frames, save the first as PNG
./build/core/hikcamtest --index 0 --frames 100 --snapshot capture.png

# Headless single grab (writes snapshot.png by default when no window)
./build/core/hikcamtest --index 0 --frames 1 --no-display
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
