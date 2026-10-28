"""Live view of camera images (internal).

The only public way to open a window is ``create_camera(..., debug_window=True)``,
which runs a :class:`DebugWindow`. It uses the X11 window of the C++ core (no
extra dependency). Images are reduced to an 8-bit grey view: colour is averaged,
the contrast is stretched from the image minimum to its maximum, and the image
is shrunk by an integer step until it fits the window.
"""

from __future__ import annotations

import math
import threading
import time
from collections.abc import Callable

import numpy as np

from . import _hik
from ._logging import LoggerAdapter
from .data import ImgData

ImageLike = ImgData | np.ndarray

#: Size (width, height) of the debug window in pixels.
WINDOW_SIZE = (960, 600)
#: Longest wait for a new frame in ms; window events are processed at least this often.
WAIT_TIMEOUT_MS = 50
#: Upper limit for redraws per second: a screen shows no more, and the 8-bit conversion holds the GIL.
MAX_DRAW_RATE_HZ = 60.0
#: How long DebugWindow() waits for its thread to open the window, in seconds.
OPEN_TIMEOUT_S = 5.0


def to_display(image: ImageLike, max_width: int, max_height: int) -> np.ndarray:
    """Convert an image to a C-contiguous (H, W) uint8 array that fits the box.

    Args:
        image: ``ImgData`` or numpy array, (H, W) or (H, W, C), any numeric dtype.
        max_width, max_height: Size of the target area in pixels.

    Returns:
        numpy.ndarray: The display image (a new array; the input is not modified).
    """
    arr = image.data if isinstance(image, ImgData) else np.asarray(image)
    if arr.ndim not in (2, 3) or arr.size == 0:
        raise ValueError(f"expected a (H, W) or (H, W, C) image, got shape {arr.shape}")
    step = max(1, math.ceil(arr.shape[0] / max_height), math.ceil(arr.shape[1] / max_width))
    # Thin out first, then average the channels: only the small image is converted.
    small = arr[::step, ::step].astype(np.float64)
    if small.ndim == 3:
        small = small.mean(axis=2)
    lo, hi = float(small.min()), float(small.max())
    if hi <= lo:
        return np.zeros(small.shape, dtype=np.uint8)
    return np.ascontiguousarray((small - lo) * (255.0 / (hi - lo)), dtype=np.uint8)


def display_available() -> bool:
    """True if the build supports windows (X11); a display may still be missing."""
    return bool(_hik.ImageWindow.display_supported())


class PreviewWindow:
    """A preview window of fixed size.

    Args:
        width, height: Window size in pixels.
        title: Window title.

    Attributes:
        is_valid (bool): False when no window could be opened (no X11 build,
            no DISPLAY); :meth:`show` then does nothing and returns True.
    """

    def __init__(self, width: int = 960, height: int = 600, title: str = "uc2_cam_py preview") -> None:
        self.width = int(width)
        self.height = int(height)
        self._window = _hik.ImageWindow(self.width, self.height, title)

    @property
    def is_valid(self) -> bool:
        return bool(self._window.is_valid)

    def show(self, image: ImageLike | None) -> bool:
        """Draw ``image`` (None: keep the current one) and process window events.

        Returns:
            bool: False once the user closed the window.
        """
        if image is not None and self._window.is_valid:
            self._window.show_gray8(to_display(image, self.width, self.height))
        return bool(self._window.pump_events())


class DebugWindow:
    """Shows the newest image of a camera in a window, on a thread of its own.

    Xlib runs without XInitThreads(), so the window is created, drawn and
    destroyed by that thread only. Nothing here raises: a missing display or a
    failure in the thread is logged as an error and the camera carries on.

    A new frame is drawn as soon as it is complete in the ring buffer (``wait``
    returns early), at most :data:`MAX_DRAW_RATE_HZ` times per second; when
    frames come faster, the newest one is shown.

    Args:
        source: Returns the newest image or None (``Camera.get_last_image``).
        wait: ``wait(after_sequence, timeout_ms)`` blocks without the GIL until a
            frame newer than ``after_sequence`` is in the ring (backend
            ``wait_for_frame``).
        title: Window title.
        log: Logger of the camera.

    Attributes:
        opened (bool): True once the window has been opened.
        frames_shown (int): Number of images drawn so far.
    """

    def __init__(
        self,
        source: Callable[[], ImgData | None],
        wait: Callable[[int, int], bool],
        title: str,
        log: LoggerAdapter,
    ) -> None:
        self._source = source
        self._wait = wait
        self._title = title
        self._log = log
        self._stop = threading.Event()
        self._tried = threading.Event()   # set once the thread has tried to open the window
        self.opened = False
        self.frames_shown = 0
        self._thread = threading.Thread(target=self._run, name="uc2_cam_py-debug-window", daemon=True)
        self._thread.start()
        # Wait for the attempt, so a missing display is logged before create_camera() returns.
        self._tried.wait(OPEN_TIMEOUT_S)

    @property
    def is_open(self) -> bool:
        """True while the window is shown (False if it never opened or was closed)."""
        return self.opened and self._thread.is_alive()

    def close(self) -> None:
        """Stop the thread; the window disappears with it."""
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _run(self) -> None:
        window = None
        try:
            window = self._open()
        except Exception as exc:  # noqa: BLE001 - the debug view must never break the camera
            self._log.error(f"debug_window=True: the window could not be opened: {exc}")
        finally:
            self._tried.set()
        if window is None:
            return
        try:
            self._loop(window)
        except Exception as exc:  # noqa: BLE001 - same reason
            self._log.error(f"debug_window: the live view stopped after an error: {exc}")
        finally:
            del window   # destroy the X window on this thread

    def _open(self) -> PreviewWindow | None:
        if not display_available():
            self._log.error("debug_window=True: this build has no X11 support - no window is shown")
            return None
        window = PreviewWindow(*WINDOW_SIZE, title=self._title)
        if not window.is_valid:
            self._log.error(
                "debug_window=True: no X11 display available (DISPLAY not set or not reachable) - "
                "no window is shown, the camera works without it"
            )
            return None
        self.opened = True
        return window

    def _loop(self, window: PreviewWindow) -> None:
        last = -1
        while not self._stop.is_set():
            started = time.monotonic()
            image = self._source()
            # "Not equal" rather than "newer": a restart (ROI, binning, start) begins again at 0.
            if image is not None and image.sequence == last:
                image = None   # nothing new: only process window events
            if not window.show(image):
                self._log.info("debug_window: window closed by the user - the camera keeps running")
                return
            if image is not None:
                last = image.sequence
                self.frames_shown += 1
                rest = 1.0 / MAX_DRAW_RATE_HZ - (time.monotonic() - started)
                if rest > 0:
                    self._stop.wait(rest)
            self._wait(last, WAIT_TIMEOUT_MS)
