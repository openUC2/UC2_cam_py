"""Session metadata (get_image_info) and recording to disk."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from uc2_cam_py import ImgMetaData, _resources

from conftest import BUFFER, wait_for_frames

SESSION_HEADER_BYTES = 8448
FRAME_HEADER_BYTES = 64


def test_no_session_info_before_start(camera) -> None:
    assert camera.get_image_info() is None


def test_session_info_describes_the_recording(camera) -> None:
    camera.start()
    wait_for_frames(camera, 1)
    meta = camera.get_image_info()
    assert isinstance(meta, ImgMetaData)
    assert meta.magic == "UC2RB"
    assert meta.header_bytes == SESSION_HEADER_BYTES
    assert (meta.width, meta.height) == camera.image_size
    assert meta.slot_count == BUFFER
    assert camera.serial in meta.camera_id
    assert abs(meta.start_wall_ns - time.time_ns()) < 60e9

    settings = json.loads(meta.comment)
    assert settings["serial"] == camera.serial
    assert settings["binning"] == camera.binning
    assert settings["exposure_time_ms"] == camera.exposure_time_ms
    assert "pixel_pitch_um" in settings

    img = camera.get_last_image()
    assert abs(meta.wall_time_ns(img.time_ns) - time.time_ns()) < 60e9


def test_recording_writes_header_frames_and_sidecar(open_camera, recording_dir) -> None:
    cam = open_camera(store_image_data=True)
    assert cam.recording_file is None
    cam.start()
    wait_for_frames(cam, 10)
    cam.stop()

    path = Path(cam.recording_file)
    assert path.parent == recording_dir
    assert path.suffix == ".uc2rb"
    saved, _dropped, error = cam._backend.stream_stats()
    assert saved > 0 and not error
    meta = cam.get_image_info()
    assert path.stat().st_size == SESSION_HEADER_BYTES + saved * (FRAME_HEADER_BYTES + meta.frame_bytes)
    sidecar = json.loads(path.with_suffix(".json").read_text())
    assert sidecar["cameraId"] == meta.camera_id


def test_every_start_opens_a_new_file(open_camera) -> None:
    cam = open_camera(store_image_data=True)
    files = []
    for _ in range(2):
        cam.start()
        wait_for_frames(cam, 2)
        cam.stop()
        files.append(cam.recording_file)
    assert files[0] != files[1]
    assert all(Path(f).exists() for f in files)


def test_store_image_data_false_writes_nothing(open_camera, recording_dir) -> None:
    cam = open_camera(store_image_data=False)
    cam.start()
    wait_for_frames(cam, 3)
    cam.stop()
    assert cam.recording_file is None
    assert not recording_dir.exists() or not any(recording_dir.iterdir())


# -------------------------------------------------- switching store_image_data

def test_store_image_data_switches_while_recording(camera, recording_dir, rec_logger) -> None:
    camera.start()
    wait_for_frames(camera, 3)
    session = camera.get_image_info().start_wall_ns
    assert camera.recording_file is None

    camera.store_image_data = True
    assert camera.store_image_data
    path = Path(camera.recording_file)
    assert path.parent == recording_dir
    wait_for_frames(camera, camera.get_last_image().sequence + 6)
    camera.store_image_data = False
    assert not camera.store_image_data

    saved, _dropped, error = camera._backend.stream_stats()
    assert saved > 0 and not error
    meta = camera.get_image_info()
    assert path.stat().st_size == SESSION_HEADER_BYTES + saved * (FRAME_HEADER_BYTES + meta.frame_bytes)

    size = path.stat().st_size
    last = camera.get_last_image().sequence
    wait_for_frames(camera, last + 4)
    assert camera.is_recording
    assert camera.get_image_info().start_wall_ns == session   # the same session: no restart
    assert camera.get_last_image().sequence > last            # its sequence just went on
    assert path.stat().st_size == size                        # switched off: nothing more written
    camera.stop()
    assert rec_logger.has("INFO", "Recording to disk started")
    assert rec_logger.has("INFO", "Recording to disk stopped")


def test_store_image_data_set_while_stopped_applies_at_start(open_camera, recording_dir) -> None:
    cam = open_camera(store_image_data=False)
    cam.store_image_data = True
    assert cam.recording_file is None
    cam.start()
    wait_for_frames(cam, 3)
    cam.stop()
    assert Path(cam.recording_file).parent == recording_dir

    cam.store_image_data = False
    cam.start()
    wait_for_frames(cam, 2)
    cam.stop()
    assert cam.recording_file is None                         # the new session stayed in RAM


def test_store_image_data_needs_a_bool_and_an_open_camera(open_camera) -> None:
    cam = open_camera()
    with pytest.raises(TypeError):
        cam.store_image_data = 1
    cam.close()
    with pytest.raises(RuntimeError):
        cam.store_image_data = True


def test_switching_on_checks_the_disk(open_camera, rec_logger, monkeypatch) -> None:
    cam = open_camera(store_image_data=False)
    cam.start()
    monkeypatch.setattr(_resources, "free_disk", lambda path: 10)
    with pytest.raises(OSError):
        cam.store_image_data = True
    assert not cam.store_image_data
    assert cam.recording_file is None and cam.is_recording
    assert rec_logger.has("ERROR", "Not enough disk space")
