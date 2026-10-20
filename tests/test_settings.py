"""Settings: write/read back, clamping, None semantics and logging (both vendors)."""

from __future__ import annotations

from pathlib import Path

import pytest

from conftest import wait_for_frames

# --------------------------------------------------------------------- exposure

def test_exposure_round_trip_and_clamping(camera, rec_logger) -> None:
    if not camera.can_set_exposure:
        pytest.skip("exposure not settable")
    lo, hi = camera.range_exposure_time_ms
    want = min(20.0, (lo + hi) / 4)
    camera.exposure_time_ms = want
    assert camera.exposure_time_ms == pytest.approx(want, rel=0.1)

    camera.exposure_time_ms = hi * 10
    assert camera.exposure_time_ms == pytest.approx(hi, rel=0.05)
    assert rec_logger.has("INFO", "clamped")

    camera.exposure_time_ms = lo / 2
    assert camera.exposure_time_ms == pytest.approx(lo, rel=0.1, abs=1e-3)


@pytest.mark.parametrize("bad, error", [(0, ValueError), (-1.0, ValueError), ("10", TypeError), (True, TypeError)])
def test_exposure_rejects_invalid_values(sim_camera, bad, error) -> None:
    with pytest.raises(error):
        sim_camera.exposure_time_ms = bad


def test_exposure_none_switches_to_auto(sim_camera, rec_logger) -> None:
    sim_camera.exposure_time_ms = None
    assert sim_camera._backend.exposure_auto()
    assert rec_logger.has("INFO", "Exposure time: automatic")
    sim_camera.exposure_time_ms = 5.0
    assert not sim_camera._backend.exposure_auto()


# ------------------------------------------------------------------- frame rate

def test_frame_rate_round_trip_and_clamping(camera) -> None:
    if not camera.can_set_frame_rate:
        pytest.skip("frame rate not settable")
    lo, hi = camera.range_frame_rate_per_sec
    want = min(10.0, (lo + hi) / 2)
    camera.frame_rate_per_sec = want
    assert camera.frame_rate_per_sec == pytest.approx(want, rel=0.1)
    camera.frame_rate_per_sec = hi * 10
    assert camera.frame_rate_per_sec == pytest.approx(hi, rel=0.05)


def test_frame_rate_none_removes_the_limit(sim_camera) -> None:
    sim_camera.frame_rate_per_sec = None
    assert sim_camera.frame_rate_per_sec == pytest.approx(200.0)   # simulated maximum
    with pytest.raises(ValueError):
        sim_camera.frame_rate_per_sec = 0


# ------------------------------------------------------------------------- gain

def test_gain_round_trip_and_clamping(camera) -> None:
    if not camera.can_set_gain:
        pytest.skip("gain not settable")
    lo, hi = camera.range_gain_db
    camera.gain_db = (lo + hi) / 2
    assert camera.gain_db == pytest.approx((lo + hi) / 2, rel=0.1, abs=0.1)
    camera.gain_db = hi + 100
    assert camera.gain_db == pytest.approx(hi, rel=0.05)


def test_gain_validation_and_auto(sim_camera) -> None:
    with pytest.raises(ValueError):
        sim_camera.gain_db = -1.0
    sim_camera.gain_db = None
    assert sim_camera._backend.gain_auto()


# ---------------------------------------------------------------------- binning

def test_binning_snaps_to_the_nearest_allowed_value(camera, rec_logger) -> None:
    if not camera.can_set_binning or camera.range_binning == [1]:
        pytest.skip("binning not settable")
    full = camera.image_size
    camera.binning = 2
    assert camera.binning == 2
    assert camera.image_size[0] == pytest.approx(full[0] / 2, abs=16)
    camera.binning = 3
    assert camera.binning in camera.range_binning
    assert rec_logger.has("INFO", "binning 3 is not supported")
    camera.binning = None
    assert camera.binning == 1


@pytest.mark.parametrize("bad, error", [(0, ValueError), (2.0, TypeError), ("2", TypeError)])
def test_binning_rejects_invalid_values(sim_camera, bad, error) -> None:
    with pytest.raises(error):
        sim_camera.binning = bad


# ---------------------------------------------------------------------- trigger

def test_trigger_is_a_placeholder(sim_camera, rec_logger) -> None:
    assert sim_camera.trigger is None
    sim_camera.trigger = None
    assert not rec_logger.messages("ERROR")
    sim_camera.trigger = "Line0"
    assert sim_camera.trigger is None
    assert rec_logger.has("ERROR", "trigger='Line0' is not supported")


# -------------------------------------------------------------------------- ROI

def test_roi_set_read_and_reset(camera) -> None:
    if not camera.can_set_roi:
        pytest.skip("ROI not settable")
    px, py = camera.pixel_count
    want = (32, 20, px // 2, py // 2)
    camera.roi = want
    got = camera.roi
    assert got is not None
    for g, w in zip(got, want):
        assert g == pytest.approx(w, abs=16)   # snapped to the camera grid
    assert camera.image_size[0] == pytest.approx(got[2] / camera.binning, abs=1)

    camera.roi = None
    assert camera.roi is None
    assert camera.image_size == (px // camera.binning, py // camera.binning)


def test_roi_is_clamped_to_the_sensor(sim_camera, rec_logger) -> None:
    sim_camera.roi = (300, 100, 1000, 1000)
    x, y, w, h = sim_camera.roi
    assert x + w <= 400 and y + h <= 200
    assert rec_logger.has("INFO", "exceeds the 400x200 sensor")


@pytest.mark.parametrize(
    "bad, error",
    [((0, 0, 10), TypeError), ((0, 0, 10.0, 10), TypeError), ("full", TypeError),
     ((-1, 0, 10, 10), ValueError), ((0, 0, 0, 10), ValueError)],
)
def test_roi_rejects_invalid_values(sim_camera, bad, error) -> None:
    with pytest.raises(error):
        sim_camera.roi = bad


# ------------------------------------------------------------- not settable

@pytest.mark.parametrize(
    "capability, setting, value, level",
    [
        ("can_set_exposure", "exposure_time_ms", 5.0, "WARNING"),
        ("can_set_frame_rate", "frame_rate_per_sec", 5.0, "WARNING"),
        ("can_set_gain", "gain_db", 3.0, "WARNING"),
        ("can_set_binning", "binning", 2, "WARNING"),
        ("can_set_roi", "roi", (0, 0, 100, 100), "WARNING"),
    ],
)
def test_not_settable_logs_and_keeps_the_value(sim_camera, rec_logger, monkeypatch,
                                               capability, setting, value, level) -> None:
    before = getattr(sim_camera, setting)
    monkeypatch.setattr(type(sim_camera), capability, property(lambda self: False))
    setattr(sim_camera, setting, value)
    assert rec_logger.has(level, "cannot be set on this camera")
    assert getattr(sim_camera, setting) == before


# ------------------------------------------------- geometry change while recording

def test_geometry_change_while_recording_restarts_the_session(open_camera, rec_logger) -> None:
    cam = open_camera(store_image_data=True)
    cam.start()
    wait_for_frames(cam, 3)
    first = cam.recording_file
    cam.binning = 2
    assert cam.is_recording
    assert rec_logger.has("INFO", "restarted for the new image geometry")
    second = cam.recording_file
    assert second != first
    wait_for_frames(cam, 2)
    assert cam.get_last_image().data.shape == (100, 200)
    cam.stop()
    assert Path(first).exists() and Path(second).exists()
