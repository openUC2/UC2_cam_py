# UC2_pyCams

An interface in Python for using the HIK Camera in UC2 Projects


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
UC2_HIKpy/
├── core/                     # the C++ library + standalone terminal app
│   ├── CMakeLists.txt        #   builds hikcam_core (lib) and HikStandAlone (app)
│   ├── include/              #   headers
│   │   ├── CaptureApp.h      #     capture session shared by CLI + Python
│   │   ├── MvsCamera.h       #     camera wrapper (enumerate/open/grab/display/save)
│   │   ├── ImageWindow.h     #     minimal X11 preview window (opaque, X11-free API)
│   │   └── MvsError.h        #     SDK error-code -> text
│   ├── src/                  #   sources (HikStandalone.cpp is the CLI; rest is the lib)
│   └── tests/                #   C++ unit tests (Catch2; -DHIK_BUILD_TESTS=ON)
├── bindings/
│   └── hik_py.cpp            # pybind11 module (_hik)
├── python/uc2_hikpy/         # the Python package
│   ├── __init__.py           #   initialises the SDK on import
│   └── __main__.py           #   `python -m uc2_hikpy` CLI
├── tests/test_hik.py         # pytest suite (+ runnable manual smoke script)
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
uv run pytest                        # runs the unit tests (no camera required)
uv run python tests/test_hik.py      # manual smoke run: lists cameras and starts a capture
```

From Python:

```python
import uc2_hikpy

for i, dev in enumerate(uc2_hikpy.list_devices()):
    print(i, dev)

# index / frames / snapshot / display are all optional
uc2_hikpy.run(index=0, frames=50, snapshot="snapshot.png", display=True)
```

`uv run python -m uc2_hikpy --index 0 --frames 100 --snapshot capture.png`
runs the same thing from the command line. The MVS runtime libraries (including
the renderer) are located automatically, so you do **not** need to export
`LD_LIBRARY_PATH`.

## Continuous grabbing (Python)

For live streaming, `Grabber` runs acquisition on an SDK worker thread and hands
Python the **latest** frame as a **zero-copy** numpy view. There is intentionally
**no ring buffer here** — an indexable history is provided by a separate external
package; this layer exposes only the most recent frame plus its handle/size so
that ring can ingest it.

```python
from uc2_hikpy import Grabber

with Grabber(index=0, node_num=5) as g:      # opens + starts; stops + closes on exit
    for frame in g.frames(timeout_ms=1000):
        img = frame.array                    # zero-copy view: (H, W) or (H, W, 3) uint8
        # frame.address / frame.nbytes / frame.width / frame.height / frame.pixel_format
        # feed the future external ring, e.g. ring.push(frame.address, frame.nbytes)
        if some_condition:
            break
```

Key points:

- **Acquisition model:** event-driven callback (`MV_CC_RegisterImageCallBackEx2`)
  on an SDK-internal thread, using the **OneByOne** grab strategy (ordered frames).
- **SDK buffer nodes:** the SDK's internal pool depth is set with
  `MV_CC_SetImageNodeNum` (default 1; some cameras 3). `Grabber(node_num=...)`
  raises it for double/triple buffering (clamped to ≥ 3).
- **Zero-copy contract:** `frame.array` aliases SDK-owned memory and is valid
  **only until the next `wait_for_frame()`** call. Call `frame.copy()` to keep an
  independent array.
- **No frames lost while the consumer keeps up.** `g.frames_dropped` counts frames
  superseded before they were read (expected ~0 with a fast consumer).

### Parameters (exposure, gain, frame rate, ROI)

Get/set are available in C++ (`hik::MvsCamera` / `hik::FrameGrabber`) and Python
(`Grabber` / the underlying `FrameGrabber`). Units: **exposure = µs, gain = dB,
frame rate = fps**.

```python
from uc2_hikpy import Grabber, AutoMode

with Grabber(index=0) as g:
    # Apply several at once (only what you pass is set; ROI is applied last):
    g.configure(exposure_us=5000, gain_db=2.0, fps=30, roi=(640, 480))

    # ...or individually:
    g.set_exposure_time(5000)          # switches auto-exposure Off
    g.set_gain(2.0)                    # switches auto-gain Off
    g.set_frame_rate(30)              # enables manual frame-rate control
    g.set_exposure_auto(AutoMode.Continuous)   # or let the firmware regulate

    print(g.get_exposure_time(), g.get_gain(), g.get_resulting_frame_rate())
    print("sensor:", g.get_sensor_size(), "roi:", g.get_roi())

    frame = g.wait_for_frame()         # frame.array.shape now matches the ROI
```

Notes:

- **Exposure / gain / frame rate change live** while grabbing.
- **ROI does not invalidate zero-copy.** `frame.array` is rebuilt from each frame's
  own dimensions, so a smaller ROI just yields a smaller zero-copy view. The SDK
  only allows ROI changes while stopped, so `set_roi()` **briefly stops and restarts**
  the stream automatically. Requested ROI values are clamped and aligned to the
  camera's increment grid (width/height step is typically 16).
- Setting a manual exposure/gain switches the matching auto mode Off so the value
  takes effect; use `set_exposure_auto()` / `set_gain_auto()` for firmware control.

## Build & run — C++ standalone (core)

```bash
cmake -S core -B build/core
cmake --build build/core -j
./build/core/HikStandAlone           # interactive: list cameras, pick one, live preview
```

At configure time CMake reports which mode it built:

- `X11 found -> building with live preview window.`
- `... building HEADLESS (no preview window).` (install `libX11-devel` and
  rebuild to switch on the window)

Options:

```
HikStandAlone [options]

  -i, --index N       Open device N (skip the interactive prompt)
  -n, --frames N      Stop after N frames (default: 0 = run until Enter / window close)
  -s, --snapshot PATH Save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif)
      --no-display    Do not open a preview window (headless)
  -h, --help          Show this help
```

```bash
# Open camera 0, grab 100 frames, save the first as PNG
./build/core/HikStandAlone --index 0 --frames 100 --snapshot capture.png

# Headless single grab (writes snapshot.png by default when no window)
./build/core/HikStandAlone --index 0 --frames 1 --no-display
```

When no preview window is available and you did not pass `--snapshot`, the
first frame is written to `snapshot.png` so you can still inspect the image.

## Tests

**Python** (pytest — no camera needed for the unit tests):

```bash
uv run pytest                 # import + list_devices smoke tests
uv run pytest -m hardware     # also runs the capture test (needs a camera)
```

**C++** (Catch2 via CTest — off by default, enable with `-DHIK_BUILD_TESTS=ON`):

```bash
cmake -S core -B build/core -DHIK_BUILD_TESTS=ON
cmake --build build/core -j
ctest --test-dir build/core --output-on-failure
```

The C++ unit tier (error-code mapping, capture options, headless `ImageWindow`,
SDK init/enumerate, pixel-format mapping, `FrameGrabber` lifecycle) needs no
camera. Add `-DHIK_ENABLE_HARDWARE_TESTS=ON` to also build the `[hardware]`
integration tests, which open the first device and grab a frame both directly
and through `FrameGrabber` (and SKIP themselves when no camera is connected).

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
