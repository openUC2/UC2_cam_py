"""Pytest suite for the continuous frame grabber.

The unit tests need no camera: without a device, opening/starting fails cleanly
and wait_for_frame returns None. The @pytest.mark.hardware test drives a real
capture and is skipped when no camera is connected.

Run:  uv run pytest tests/test_grabber.py
      uv run pytest -m hardware        # with a camera attached
"""

from __future__ import annotations

import numpy as np
import pytest
import uc2_hikpy


def test_grabber_symbols_exist() -> None:
    """The grabber API is exported from the package."""
    assert hasattr(uc2_hikpy, "Grabber")
    assert hasattr(uc2_hikpy, "FrameGrabber")
    assert hasattr(uc2_hikpy, "Frame")


def test_fresh_framegrabber_is_idle() -> None:
    """A newly constructed low-level grabber is not running and has no frames."""
    g = uc2_hikpy.FrameGrabber()
    assert g.is_running is False
    assert g.frames_received == 0
    assert g.frames_dropped == 0
    assert g.width == 0 and g.height == 0


def test_start_without_camera_returns_false() -> None:
    """Grabber.start() fails cleanly (no exception) when no camera is present."""
    if uc2_hikpy.list_devices():
        pytest.skip("a camera is connected; covered by the hardware test")
    g = uc2_hikpy.Grabber(index=0)
    assert g.start() is False
    assert g.wait_for_frame(10) is None
    g.close()


@pytest.mark.hardware
def test_grabber_streams_a_frame() -> None:
    """End-to-end zero-copy grab on the first device. Skipped without hardware."""
    if not uc2_hikpy.list_devices():
        pytest.skip("no HikRobot/MVS camera connected")

    with uc2_hikpy.Grabber(index=0, node_num=5) as g:
        frame = g.wait_for_frame(2000)
        assert frame is not None
        assert frame.width > 0 and frame.height > 0
        assert frame.address != 0

        arr = frame.array
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.uint8
        assert arr.nbytes == frame.nbytes
        # The zero-copy view must alias the reported buffer address.
        assert arr.__array_interface__["data"][0] == frame.address

        assert g.frames_received > 0
