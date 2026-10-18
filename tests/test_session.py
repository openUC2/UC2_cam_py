"""Session metadata (get_image_info) and recording to disk."""

from __future__ import annotations

import json
import time
from pathlib import Path

from uc2_cam_py import ImgMetaData

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
    saved, dropped, error = cam._backend.stream_stats()
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
