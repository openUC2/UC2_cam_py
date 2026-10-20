"""Live view of camera images: ``create_camera(..., debug_window=True)``.

The ``display`` tests open the debug window and let it show the newest image for
a few seconds (``UC2_DISPLAY_SECONDS``, default 5). With the simulated camera
the Gaussian spot walks over the sensor in four rows. They are skipped without a
display. ``-m "display and hardware"`` runs the same on a real camera. The other
tests need no display.

    UC2_DISPLAY_SECONDS=30 uv run pytest -m display tests/test_display.py
"""

from __future__ import annotations

import os
import time

import numpy as np
import pytest

from uc2_cam_py import ImgData
from uc2_cam_py._preview import to_display

from conftest import hik_camera_connected, wait_for_frames

SECONDS = float(os.environ.get("UC2_DISPLAY_SECONDS", "5"))


def _live_view(cam) -> int:
    """Record for SECONDS while the debug window shows the images; return how many it drew."""
    window = cam._debug_window
    if window is None or not window.opened:
        pytest.skip("debug window could not be opened")
    cam.start()
    wait_for_frames(cam, 1)
    time.sleep(SECONDS)
    cam.stop()
    return window.frames_shown


# ------------------------------------------------------------ without display

def test_to_display_scales_and_fits() -> None:
    img = np.zeros((1200, 1920), np.uint16)
    img[600, 960] = 60000
    out = to_display(img, 960, 600)
    assert out.dtype == np.uint8
    assert out.shape == (600, 960)
    assert out.flags.c_contiguous
    assert out.max() == 255 and out.min() == 0


def test_to_display_accepts_imgdata_rgb_and_flat_images() -> None:
    rgb = np.full((10, 20, 3), 7, np.uint8)
    assert to_display(rgb, 100, 100).shape == (10, 20)
    assert not to_display(rgb, 100, 100).any()           # flat image -> black, no division by 0
    data = ImgData(np.arange(12, dtype=np.uint16).reshape(3, 4), 0, 0, 0, 0, 0)
    assert to_display(data, 100, 100).max() == 255
    with pytest.raises(ValueError):
        to_display(np.zeros(5), 10, 10)


def test_no_debug_window_by_default(open_camera) -> None:
    assert open_camera()._debug_window is None


def test_debug_window_without_display_logs_an_error(open_camera, rec_logger, monkeypatch) -> None:
    monkeypatch.delenv("DISPLAY", raising=False)
    monkeypatch.delenv("WAYLAND_DISPLAY", raising=False)
    cam = open_camera(debug_window=True)                  # must not raise
    assert not cam._debug_window.opened
    assert rec_logger.has("ERROR", "debug_window=True")
    cam.start()
    wait_for_frames(cam, 5)                               # the camera works without the window


# ---------------------------------------------------------------- with display

@pytest.mark.display
def test_debug_window_shows_the_simulated_camera(open_camera) -> None:
    cam = open_camera(sensor_width=1920, sensor_height=1200, frame_rate=10.0, debug_window=True)
    window = cam._debug_window
    assert _live_view(cam) >= min(10, int(SECONDS * 10 * 0.5))
    cam.close()
    assert cam._debug_window is None
    assert not window._thread.is_alive()                 # close() ends the window thread


@pytest.mark.display
def test_debug_window_with_roi_and_binning(open_camera) -> None:
    cam = open_camera(sensor_width=1920, sensor_height=1200, frame_rate=10.0, debug_window=True)
    cam.roi = (192, 150, 1536, 900)
    cam.binning = 2
    assert _live_view(cam) >= min(10, int(SECONDS * 10 * 0.5))
    assert cam.get_last_image().data.shape == (450, 768)


@pytest.mark.display
@pytest.mark.hardware
def test_debug_window_shows_the_hikrobot_camera(open_camera) -> None:
    if not hik_camera_connected():
        pytest.skip("no HikRobot camera connected")
    cam = open_camera("HikRobot", debug_window=True)
    assert _live_view(cam) >= 3
