"""Forwarding of the C++ log messages to the Python logger."""

from __future__ import annotations

import logging
import threading
import time

import pytest

import uc2_cam_py
from uc2_cam_py import _hik, _logging

from conftest import SIM_OPTIONS, wait_for_frames


@pytest.mark.parametrize(
    "level, name",
    [(logging.DEBUG, "DEBUG"), (logging.INFO, "INFO"), (logging.WARNING, "WARNING"), (logging.ERROR, "ERROR")],
)
def test_cpp_messages_reach_the_camera_logger(open_camera, rec_logger, level, name) -> None:
    open_camera()
    _hik._emit_log(level, f"hello from C++ at {name}")
    assert rec_logger.has(name, f"hello from C++ at {name}")


def test_plain_logging_logger_is_used_by_default(tmp_path, caplog) -> None:
    caplog.set_level(logging.DEBUG, logger="uc2_cam_py")
    cam = uc2_cam_py.create_camera(vendor="Simulated", store_image_data=False, **SIM_OPTIONS)
    try:
        _logging.flush()
        assert any("Camera initialised" in r.getMessage() for r in caplog.records)
        assert all(r.name == "uc2_cam_py" for r in caplog.records)
    finally:
        cam.close()


def test_logger_with_warn_instead_of_warning_is_accepted(tmp_path) -> None:
    class OldStyle:
        def __init__(self):
            self.warned = []

        def error(self, m): pass
        def warn(self, m): self.warned.append(m)
        def info(self, m): pass
        def debug(self, m): pass

    log = OldStyle()
    with uc2_cam_py.create_camera(vendor="Simulated", store_image_data=False, logger=log, **SIM_OPTIONS):
        _hik._emit_log(logging.WARNING, "via warn()")
        _logging.flush()
    assert "via warn()" in log.warned


def test_incomplete_logger_is_rejected() -> None:
    class NoDebug:
        def error(self, m): pass
        def warning(self, m): pass
        def info(self, m): pass

    with pytest.raises(TypeError, match="debug"):
        uc2_cam_py.create_camera(vendor="Simulated", store_image_data=False, logger=NoDebug())


def test_camera_thread_does_not_need_the_gil(open_camera) -> None:
    """While a Python thread hogs the GIL, the camera keeps delivering frames."""
    cam = open_camera(start_on_init=True, frame_rate=100.0)
    wait_for_frames(cam, 1)
    before = cam._backend.frames_written()
    end = time.monotonic() + 0.5
    x = 0
    while time.monotonic() < end:     # pure-Python loop: holds the GIL
        x += 1
    after = cam._backend.frames_written()
    assert after - before >= 20       # ~50 expected at 100 fps


def test_no_deadlock_when_stopping_under_log_load(open_camera) -> None:
    cam = open_camera(frame_rate=200.0)
    stop_spam = threading.Event()

    def spam() -> None:
        while not stop_spam.is_set():
            _hik._emit_log(logging.DEBUG, "load")

    spammer = threading.Thread(target=spam, daemon=True)
    spammer.start()
    done = threading.Event()

    def cycle() -> None:
        for _ in range(5):
            cam.start()
            wait_for_frames(cam, 2)
            cam.stop()
        done.set()

    worker = threading.Thread(target=cycle, daemon=True)
    worker.start()
    worker.join(timeout=30)
    stop_spam.set()
    spammer.join(timeout=5)
    assert done.is_set(), "start/stop cycles did not finish - deadlock?"
    assert _logging.flush()
