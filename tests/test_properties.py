"""Read-only properties: types, units and consistency (both vendors)."""

from __future__ import annotations

import pytest

from uc2_cam_py import BINNING_FACTORS, CONNECTION_TYPES

from conftest import BUFFER, wait_for_frames


def _check_range(rng, settable: bool) -> None:
    if not settable:
        assert rng is None
        return
    assert isinstance(rng, tuple) and len(rng) == 2
    lo, hi = rng
    assert isinstance(lo, float) and isinstance(hi, float)
    assert 0.0 <= lo < hi


def test_capabilities_are_bools(camera) -> None:
    for name in ("can_set_gain", "can_set_frame_rate", "can_set_binning", "can_set_trigger",
                 "can_set_exposure", "can_set_roi"):
        assert isinstance(getattr(camera, name), bool), name


def test_ranges_follow_the_capabilities(camera) -> None:
    _check_range(camera.range_exposure_time_ms, camera.can_set_exposure)
    _check_range(camera.range_frame_rate_per_sec, camera.can_set_frame_rate)
    _check_range(camera.range_gain_db, camera.can_set_gain)


def test_range_binning(camera) -> None:
    values = camera.range_binning
    assert isinstance(values, list) and values
    assert all(isinstance(b, int) and b in BINNING_FACTORS for b in values)
    if not camera.can_set_binning:
        assert values == [1]


def test_pixel_geometry(camera) -> None:
    px = camera.pixel_count
    assert isinstance(px, tuple) and len(px) == 2
    assert all(isinstance(v, int) and v > 0 for v in px)

    pitch = camera.pixel_pitch_um
    size = camera.pixel_size_um
    if pitch is None:
        assert size is None
    else:
        assert all(v > 0 for v in pitch)
        assert size is not None and all(0 < s <= p for s, p in zip(size, pitch))


def test_identity_properties(camera) -> None:
    assert camera.connection_type in CONNECTION_TYPES
    assert camera.vendor in ("Simulated", "HikRobot")
    assert isinstance(camera.serial, str) and camera.serial
    assert isinstance(camera.index, int) and camera.index >= 0
    assert camera.buffer_size == BUFFER


def test_is_recording_follows_start_and_stop(camera) -> None:
    assert camera.is_recording is False
    camera.start()
    assert camera.is_recording is True
    wait_for_frames(camera, 1)
    camera.stop()
    assert camera.is_recording is False


def test_simulated_values(sim_camera) -> None:
    assert sim_camera.pixel_count == (400, 200)
    assert sim_camera.pixel_pitch_um == (5.86, 5.86)
    assert sim_camera.pixel_size_um == (5.86, 5.86)       # no size -> pitch
    assert sim_camera.range_exposure_time_ms == (0.01, 1000.0)
    assert sim_camera.range_gain_db == (0.0, 24.0)
    assert sim_camera.range_binning == [1, 2, 4]
    assert sim_camera.can_set_trigger is False
    assert sim_camera.model == "UC2-SIM"
    assert sim_camera.pixel_format == "Mono16"
    assert sim_camera.image_size == (400, 200)


@pytest.mark.hardware
def test_hikrobot_pixel_pitch_is_none_without_model_entry(open_camera) -> None:
    from uc2_cam_py import hikrobot

    cam = open_camera("HikRobot")
    if cam.model in hikrobot.MODELS:
        pytest.skip("model has a table entry")
    assert cam.pixel_pitch_um is None
    assert cam.pixel_size_um is None
