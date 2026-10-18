"""Live display of camera images through get_last_image().

The ``display`` tests open a preview window and show the newest image for a
few seconds (``UC2_DISPLAY_SECONDS``, default 5). With the simulated camera the
Gaussian spot walks over the sensor in four rows. They are skipped without a
display. ``-m "display and hardware"`` runs the same on a real camera.

    UC2_DISPLAY_SECONDS=30 uv run pytest -m display tests/test_display.py
"""

from __future__ import annotations

import os
import time

import numpy as np
import pytest

from uc2_cam_py import ImgData
from uc2_cam_py.preview import PreviewWindow, show, to_display

from conftest import hik_camera_connected, wait_for_frames

SECONDS = float(os.environ.get("UC2_DISPLAY_SECONDS", "5"))


def _live_view(cam, title: str) -> set:
    """Show get_last_image() in a window for SECONDS; return the sequences shown."""
    win = PreviewWindow(960, 600, title)
    if not win.is_valid:
        pytest.skip("preview window could not be opened")
    cam.start()
    wait_for_frames(cam, 1)
    shown = set()
    end = time.monotonic() + SECONDS
    while time.monotonic() < end:
        img = cam.get_last_image()
        if img is not None:
            if not show(img, win):
                break                   # window closed by the user
            shown.add(img.sequence)
        time.sleep(0.02)
    cam.stop()
    return shown


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


# ---------------------------------------------------------------- with display

@pytest.mark.display
def test_live_view_of_the_simulated_camera(open_camera) -> None:
    cam = open_camera(sensor_width=1920, sensor_height=1200, frame_rate=10.0)
    shown = _live_view(cam, "uc2_cam_py - Simulated, full sensor")
    assert len(shown) >= min(10, int(SECONDS * 10 * 0.5))


@pytest.mark.display
def test_live_view_with_roi_and_binning(open_camera) -> None:
    cam = open_camera(sensor_width=1920, sensor_height=1200, frame_rate=10.0)
    cam.roi = (192, 150, 1536, 900)
    cam.binning = 2
    shown = _live_view(cam, "uc2_cam_py - Simulated, ROI + binning 2")
    assert len(shown) >= min(10, int(SECONDS * 10 * 0.5))
    assert cam.get_last_image().data.shape == (450, 768)


@pytest.mark.display
@pytest.mark.hardware
def test_live_view_of_the_hikrobot_camera(open_camera) -> None:
    if not hik_camera_connected():
        pytest.skip("no HikRobot camera connected")
    cam = open_camera("HikRobot")
    shown = _live_view(cam, f"uc2_cam_py - HikRobot {cam.model}")
    assert len(shown) >= 3
