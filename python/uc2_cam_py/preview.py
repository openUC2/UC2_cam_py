"""Minimal live preview for images of uc2_cam_py.

Uses the X11 window of the C++ core (no extra dependency). Images are reduced
to an 8-bit grey view: colour is averaged, the contrast is stretched from the
image minimum to its maximum, and the image is shrunk by an integer step until
it fits the window.

Example:
    from uc2_cam_py import create_camera
    from uc2_cam_py.preview import PreviewWindow

    with create_camera(vendor="Simulated", start_on_init=True) as cam:
        win = PreviewWindow()
        while win.show(cam.get_last_image()):
            pass                         # until the window is closed
"""

from __future__ import annotations

import math
from typing import Optional, Union

import numpy as np

from . import _hik
from .data import ImgData

ImageLike = Union[ImgData, np.ndarray]


def to_display(image: ImageLike, max_width: int, max_height: int) -> np.ndarray:
    """Convert an image to a C-contiguous (H, W) uint8 array that fits the box.

    Args:
        image: ``ImgData`` or numpy array, (H, W) or (H, W, C), any numeric dtype.
        max_width, max_height: Size of the target area in pixels.

    Returns:
        numpy.ndarray: The display image (a new array; the input is not modified).
    """
    arr = image.data if isinstance(image, ImgData) else np.asarray(image)
    if arr.ndim == 3:
        arr = arr.mean(axis=2)
    if arr.ndim != 2 or arr.size == 0:
        raise ValueError(f"expected a (H, W) or (H, W, C) image, got shape {arr.shape}")
    step = max(1, math.ceil(arr.shape[0] / max_height), math.ceil(arr.shape[1] / max_width))
    small = arr[::step, ::step].astype(np.float64)
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

    def show(self, image: Optional[ImageLike]) -> bool:
        """Draw ``image`` (None: keep the current one) and process window events.

        Returns:
            bool: False once the user closed the window.
        """
        if image is not None and self._window.is_valid:
            self._window.show_gray8(to_display(image, self.width, self.height))
        return bool(self._window.pump_events())


def show(image: Optional[ImageLike], window: PreviewWindow) -> bool:
    """Function form of :meth:`PreviewWindow.show`."""
    return window.show(image)
