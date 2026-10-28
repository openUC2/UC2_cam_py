"""Image access: get_last_image / get_image, zero-copy views and image content."""

from __future__ import annotations

import time

import numpy as np
import pytest

from uc2_cam_py import ImgData

from conftest import BUFFER, wait_for_frames


def _centroid(img: np.ndarray):
    """Centre of the spot. The image has an offset and noise, so only the spot is weighed:
    background = median, weight = max(v - background - 0.2 * height, 0). Unbiased for a
    symmetric spot."""
    img = img.astype(np.float64)
    bg = np.median(img)
    weight = np.clip(img - bg - 0.2 * (img.max() - bg), 0.0, None)
    total = weight.sum()
    ys, xs = np.indices(img.shape)
    return float((weight * xs).sum() / total), float((weight * ys).sum() / total)


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
    assert a.data.shape == (camera.image_size[1], camera.image_size[0], 1)
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
    assert img.data.shape == (200, 400, 1)


@pytest.mark.parametrize("separate_process", [True, False], ids=["process", "in-process"])
@pytest.mark.parametrize("pixel_format, itemsize", [("Mono8", 1), ("Mono16", 2)])
def test_data_is_always_three_dimensional(open_camera, separate_process, pixel_format, itemsize) -> None:
    """One channel comes as (H, W, 1) - the layout of a colour image - and stays zero-copy."""
    cam = open_camera(start_on_init=True, pixel_format=pixel_format, separate_process=separate_process)
    wait_for_frames(cam, 1)
    data = cam.get_last_image().data
    w, h = cam.image_size
    assert data.shape == (h, w, 1)
    assert data.strides == (w * itemsize, itemsize, itemsize)
    plane = data[..., 0]                             # the 2D view of the channel ...
    assert plane.shape == (h, w)
    assert np.shares_memory(plane, data)             # ... is no copy
    assert not plane.flags.writeable


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
    cam.stop()                               # nothing can overwrite the slot any more
    img = cam.get_last_image()
    before = img.data.copy()
    cam.close()
    del cam
    assert np.array_equal(img.data, before)  # the ring memory is still alive and intact


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
    cx, cy = _centroid(img.data[..., 0])
    ex, ey = sim_camera.spot_position(img.sequence)
    assert cx == pytest.approx(ex, abs=0.5)
    assert cy == pytest.approx(ey, abs=0.5)


@pytest.mark.parametrize("pixel_format", ["Mono16", "Mono8"])
def test_simulated_image_has_an_offset_with_shot_noise(open_camera, pixel_format) -> None:
    cam = open_camera(pixel_format=pixel_format)
    cam.start()
    wait_for_frames(cam, 2)
    cam.stop()
    img = cam.get_last_image()
    data = img.data[..., 0].astype(np.float64)
    ex, ey = cam.spot_position(img.sequence)
    background = float(np.median(data))
    # Mean of the 3 x 3 pixels at the spot centre: the maximum would be the top of the noise.
    x, y = round(ex), round(ey)
    height = float(data[y - 1 : y + 2, x - 1 : x + 2].mean()) - background
    assert background == pytest.approx(0.1 * height, rel=0.1)    # offset: 10 % of the spot height

    # Away from the spot (a third of the shorter side, several spot widths) only the offset is left.
    ys, xs = np.indices(data.shape)
    far = data[np.hypot(xs - ex, ys - ey) > min(data.shape) / 3]
    assert far.var() == pytest.approx(far.mean(), rel=0.05)      # shot noise: variance = mean


def test_image_content_with_roi_and_binning(sim_camera) -> None:
    sim_camera.binning = 2
    sim_camera.start()
    wait_for_frames(sim_camera, 3)
    sim_camera.stop()
    img = sim_camera.get_last_image()
    assert img.data.shape == (100, 200, 1)
    cx, cy = _centroid(img.data[..., 0])
    ex, ey = sim_camera.spot_position(img.sequence)
    # binned pixel i covers sensor pixels 2i..2i+1 -> sensor x = 2*cx + 0.5
    assert 2 * cx + 0.5 == pytest.approx(ex, abs=1.0)
    assert 2 * cy + 0.5 == pytest.approx(ey, abs=1.0)
