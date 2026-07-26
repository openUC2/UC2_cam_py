"""Pytest suite for camera parameter control.

The no-hardware tests just check the API surface. The @pytest.mark.hardware test
drives a real camera: it sets exposure/gain/frame-rate, reads them back, and
verifies that an ROI change keeps the zero-copy policy intact.

Run:  uv run pytest tests/test_params.py
      uv run pytest -m hardware        # with a camera attached
"""

from __future__ import annotations

import pytest
import uc2_hikpy


def test_param_symbols_exist() -> None:
    """AutoMode and Roi are exported and have the expected members."""
    assert hasattr(uc2_hikpy, "AutoMode")
    assert hasattr(uc2_hikpy, "Roi")
    assert uc2_hikpy.AutoMode.Off is not None
    assert uc2_hikpy.AutoMode.Once is not None
    assert uc2_hikpy.AutoMode.Continuous is not None


@pytest.mark.hardware
def test_exposure_gain_framerate_roundtrip() -> None:
    """Set and read back exposure, gain and frame rate on a real camera."""
    if not uc2_hikpy.list_devices():
        pytest.skip("no HikRobot/MVS camera connected")

    with uc2_hikpy.Grabber(index=0) as g:
        emin, emax = g.get_exposure_time_range()
        want = min(20000.0, (emin + emax) * 0.25)
        assert g.set_exposure_time(want)
        assert g.get_exposure_time() == pytest.approx(want, rel=0.1)

        assert g.set_exposure_auto(uc2_hikpy.AutoMode.Off)
        assert g.get_exposure_auto() == uc2_hikpy.AutoMode.Off

        gmin, _ = g.get_gain_range()
        assert g.set_gain(gmin)
        assert g.get_gain() == pytest.approx(gmin, abs=0.5)

        assert g.set_frame_rate(15.0)
        assert g.get_frame_rate() == pytest.approx(15.0, rel=0.2)

        w_max, h_max = g.get_sensor_size()
        assert w_max > 0 and h_max > 0


@pytest.mark.hardware
def test_roi_preserves_zero_copy() -> None:
    """A smaller ROI still yields a correctly-shaped zero-copy frame."""
    if not uc2_hikpy.list_devices():
        pytest.skip("no HikRobot/MVS camera connected")

    with uc2_hikpy.Grabber(index=0) as g:
        assert g.set_roi(640, 480)          # restarts the stream internally
        frame = g.wait_for_frame(2000)
        assert frame is not None
        assert frame.width <= 640 and frame.height <= 480
        assert frame.width > 0 and frame.height > 0

        arr = frame.array
        assert arr.shape[:2] == (frame.height, frame.width)
        # Zero-copy must survive the ROI change.
        assert arr.__array_interface__["data"][0] == frame.address

        roi = g.get_roi()
        assert roi.width == frame.width and roi.height == frame.height
