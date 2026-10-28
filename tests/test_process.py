"""Cameras in their own process - ``create_camera(..., separate_process=True)``, the default.

The interface tests run on this path anyway (``camera`` fixture); these tests
cover what only exists here: one process per camera, its end, its death, and
the routing of its log messages.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import numpy as np
import pytest

import uc2_cam_py

from conftest import SIM_OPTIONS, RecordingLogger, wait_for_frames

#: Size of one frame record in a .uc2rb file of the SIM_OPTIONS camera (Mono16).
FRAME_RECORD = 64 + SIM_OPTIONS["sensor_width"] * SIM_OPTIONS["sensor_height"] * 2
SESSION_HEADER = 8448


def gone(pid: int, timeout: float = 5.0) -> bool:
    """True once process ``pid`` has ended and been collected."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if not Path(f"/proc/{pid}").exists():
            return True
        time.sleep(0.01)
    return not Path(f"/proc/{pid}").exists()


def comm(pid: int) -> str:
    return Path(f"/proc/{pid}/comm").read_text().strip()


def test_each_camera_runs_in_a_process_of_its_own(open_camera) -> None:
    a = open_camera(start_on_init=True)
    b = open_camera(start_on_init=True)
    assert a.process_id is not None and b.process_id is not None
    assert len({a.process_id, b.process_id, os.getpid()}) == 3
    assert comm(a.process_id) == "uc2cam-SIM-0001"   # visible as such in top/htop
    wait_for_frames(a, 3)
    wait_for_frames(b, 3)
    img = a.get_last_image()
    assert img is not None and img.data.shape == (SIM_OPTIONS["sensor_height"], SIM_OPTIONS["sensor_width"], 1)


def test_separate_process_false_keeps_the_camera_in_this_process(open_camera) -> None:
    cam = open_camera(separate_process=False, start_on_init=True)
    assert cam.process_id is None
    wait_for_frames(cam, 2)


def test_defaults_decide_when_nothing_is_given(monkeypatch, open_camera) -> None:
    monkeypatch.setattr(uc2_cam_py.defaults, "SEPARATE_PROCESS", False)
    assert open_camera().process_id is None


def test_close_ends_the_camera_process(open_camera) -> None:
    cam = open_camera(start_on_init=True)
    wait_for_frames(cam, 2)
    pid = cam.process_id
    assert pid is not None
    img = cam.get_last_image()
    cam.close()
    assert gone(pid, timeout=0.1)   # already collected when close() returns
    assert img is not None and int(np.sum(img.data, dtype=np.uint64)) > 0   # images stay valid


def test_a_killed_camera_process_does_not_take_the_application_down(open_camera, rec_logger) -> None:
    cam = open_camera(start_on_init=True)
    wait_for_frames(cam, 3)
    img = cam.get_last_image()
    assert img is not None
    pid = cam.process_id
    assert pid is not None

    os.kill(pid, signal.SIGKILL)
    end = time.monotonic() + 5
    while cam.is_recording and time.monotonic() < end:
        time.sleep(0.01)
    assert not cam.is_recording

    # The image's memory belongs to this process: reading it cannot fault.
    assert int(np.sum(img.data, dtype=np.uint64)) > 0
    assert cam.get_last_image() is not None   # the last session stays readable
    assert rec_logger.has("ERROR", "the camera is no longer available")
    assert rec_logger.has("ERROR", "killed by signal 9")
    with pytest.raises(RuntimeError):
        cam.start()
    with pytest.raises(RuntimeError):
        cam.exposure_time_ms = 5.0

    started = time.monotonic()
    cam.close()
    assert time.monotonic() - started < 2.0
    assert gone(pid, timeout=0.1)


def test_messages_of_each_camera_process_reach_its_own_logger(recording_dir) -> None:
    def open_with(logger: RecordingLogger) -> uc2_cam_py.Camera:
        return uc2_cam_py.create_camera(
            vendor="Simulated", store_image_data=False, image_file_path=recording_dir, logger=logger,
            sensor_width=400, sensor_height=200, frame_rate=100.0,
        )

    log_a, log_b = RecordingLogger(), RecordingLogger()
    a = open_with(log_a)
    b = open_with(log_b)   # b is now the most recent camera
    try:
        a.start()   # logged in a's camera process
        a.stop()
    finally:
        a.close()
        b.close()
    started = [m for m in log_a.messages("INFO") if "Acquisition started" in m]
    assert len(started) == 1
    assert not [m for m in log_b.messages("INFO") if "Acquisition started" in m]


def test_sigint_leaves_closing_to_the_application(open_camera) -> None:
    """Ctrl+C in a terminal reaches the camera processes too; they ignore it."""
    cam = open_camera(start_on_init=True)
    wait_for_frames(cam, 2)
    pid = cam.process_id
    assert pid is not None
    os.kill(pid, signal.SIGINT)
    time.sleep(0.2)
    assert cam.is_recording
    img = cam.get_last_image()
    assert img is not None
    wait_for_frames(cam, img.sequence + 5)


def test_the_camera_process_ends_with_the_application_and_completes_its_file(tmp_path) -> None:
    """The application dies without closing anything: the camera process notices and cleans up."""
    script = textwrap.dedent(
        f"""
        import os, sys, time
        import uc2_cam_py
        cam = uc2_cam_py.create_camera(
            vendor="Simulated", image_file_path={str(tmp_path)!r}, store_image_data=True,
            start_on_init=True, sensor_width={SIM_OPTIONS['sensor_width']},
            sensor_height={SIM_OPTIONS['sensor_height']}, frame_rate=100.0)
        time.sleep(0.5)
        print(cam.process_id, flush=True)
        os._exit(0)   # no close(), no atexit, no destructors
        """
    )
    out = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    pid = int(out.stdout.split()[-1])
    assert gone(pid)

    files = list(tmp_path.glob("*.uc2rb"))
    assert len(files) == 1
    size = files[0].stat().st_size
    assert size > SESSION_HEADER
    assert (size - SESSION_HEADER) % FRAME_RECORD == 0   # whole records only: the file was completed
