"""Image access: get_last_image / get_image, zero-copy views and image content."""

from __future__ import annotations

import time

import numpy as np
import pytest

from uc2_cam_py import ImgData

from conftest import BUFFER, wait_for_frames


def _centroid(img: np.ndarray):
    img = img.astype(np.float64)
    total = img.sum()
    ys, xs = np.indices(img.shape)
    return float((img * xs).sum() / total), float((img * ys).sum() / total)


def test_nothing_before_the_first_start(camera) -> None:
    assert camera.get_last_image() is None
    with pytest.raises(LookupError):
        camera.get_image(0)


@pytest.mark.parametrize("index, error", [(BUFFER, IndexError), (-1, IndexError), (1.0, TypeError), ("0", TypeError)])
def test_get_image_rejects_bad_indices(sim_camera, index, error) -> None:
    with pytest.raises(error):
        sim_camera.get_image(index)


def test_last_image_is_imgdata_with_consistent_times(camera) -> None:
    camera.start()
    wait_for_frames(camera, 3)
    a = camera.get_last_image()
    assert isinstance(a, ImgData)
    assert a.time_before_ns <= a.time_after_ns
    assert a.time_ns == (a.time_before_ns + a.time_after_ns) // 2
    assert a.slot == a.sequence % BUFFER
    assert a.data.shape[:2] == (camera.image_size[1], camera.image_size[0])
    time.sleep(0.1)
    b = camera.get_last_image()
    assert b.sequence > a.sequence
    assert b.time_before_ns > a.time_before_ns


def test_dtype_follows_the_pixel_format(open_camera) -> None:
    mono16 = open_camera(start_on_init=True)
    wait_for_frames(mono16, 1)
    assert mono16.get_last_image().data.dtype == np.uint16

    mono8 = open_camera(start_on_init=True, pixel_format="Mono8")
    wait_for_frames(mono8, 1)
    img = mono8.get_last_image()
    assert img.data.dtype == np.uint8
    assert img.data.shape == (200, 400)


def test_view_is_zero_copy_and_read_only(sim_camera) -> None:
    sim_camera.start()
    wait_for_frames(sim_camera, 2)
    sim_camera.stop()
    img = sim_camera.get_last_image()
    assert img.data.ctypes.data == img._handle.address        # points into the ring slot
    again = sim_camera.get_image(img.slot)
    assert again.data.ctypes.data == img.data.ctypes.data      # same memory, no copy
    assert not img.data.flags.writeable
    with pytest.raises(ValueError):
        img.data[0, 0] = 1
    assert img.is_valid()


def test_every_slot_can_be_read_after_a_full_lap(sim_camera) -> None:
    sim_camera.start()
    wait_for_frames(sim_camera, BUFFER + 2)
    sim_camera.stop()
    for slot in range(BUFFER):
        img = sim_camera.get_image(slot)
        assert img.slot == slot
        assert img.sequence % BUFFER == slot


def test_view_survives_close(open_camera) -> None:
    cam = open_camera(start_on_init=True)
    wait_for_frames(cam, 2)
    img = cam.get_last_image()
    cam.close()
    del cam
    assert int(img.data.max()) == 60000      # the ring memory is still alive and intact


def test_overwritten_slot_is_reported_invalid(open_camera) -> None:
    cam = open_camera(buffer_size=2, frame_rate=200.0)
    cam.start()
    wait_for_frames(cam, 1)
    img = cam.get_last_image()
    wait_for_frames(cam, img.sequence + 4)   # two more laps of a 2-slot ring
    assert not img.is_valid()


def test_image_content_matches_the_simulated_spot(sim_camera) -> None:
    sim_camera.start()
    wait_for_frames(sim_camera, 5)
    sim_camera.stop()                        # freeze the ring: nothing can be recycled
    img = sim_camera.get_last_image()
    cx, cy = _centroid(img.data)
    ex, ey = sim_camera.spot_position(img.sequence)
    assert cx == pytest.approx(ex, abs=0.5)
    assert cy == pytest.approx(ey, abs=0.5)


def test_image_content_with_roi_and_binning(sim_camera) -> None:
    sim_camera.binning = 2
    sim_camera.start()
    wait_for_frames(sim_camera, 3)
    sim_camera.stop()
    img = sim_camera.get_last_image()
    assert img.data.shape == (100, 200)
    cx, cy = _centroid(img.data)
    ex, ey = sim_camera.spot_position(img.sequence)
    # binned pixel i covers sensor pixels 2i..2i+1 -> sensor x = 2*cx + 0.5
    assert 2 * cx + 0.5 == pytest.approx(ex, abs=1.0)
    assert 2 * cy + 0.5 == pytest.approx(ey, abs=1.0)
