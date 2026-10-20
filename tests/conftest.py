"""Shared fixtures for the uc2_cam_py test suite.

Most interface tests run against both vendors through the ``camera`` fixture:
"Simulated" always, "HikRobot" only with a connected camera (marked
``hardware``, skipped otherwise). Run:

    uv run pytest -m "not hardware"          # everything without a camera
    uv run pytest -m hardware                # the same checks on a real camera
    UC2_DISPLAY_SECONDS=30 uv run pytest -m display    # live view, longer
"""

from __future__ import annotations

import os
import threading
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

import uc2_cam_py
from uc2_cam_py import _logging
from uc2_cam_py._preview import display_available

#: Small, fast simulated camera used by most tests.
SIM_OPTIONS = dict(sensor_width=400, sensor_height=200, frame_rate=100.0)
BUFFER = 8


class RecordingLogger:
    """Logger-like object (deliberately not a logging.Logger) that records calls."""

    def __init__(self) -> None:
        self.records: list[tuple[str, str]] = []
        self._lock = threading.Lock()

    def _add(self, level: str, message: str) -> None:
        with self._lock:
            self.records.append((level, str(message)))

    def error(self, message: str) -> None:
        self._add("ERROR", message)

    def warning(self, message: str) -> None:
        self._add("WARNING", message)

    def info(self, message: str) -> None:
        self._add("INFO", message)

    def debug(self, message: str) -> None:
        self._add("DEBUG", message)

    def messages(self, level: str | None = None) -> list[str]:
        """Messages so far (C++ messages included: the forwarder is flushed first)."""
        _logging.flush()
        with self._lock:
            return [m for lv, m in self.records if level is None or lv == level]

    def has(self, level: str, text: str) -> bool:
        return any(text in m for m in self.messages(level))

    def clear(self) -> None:
        _logging.flush()
        with self._lock:
            self.records.clear()


def hik_camera_connected() -> bool:
    return bool(uc2_cam_py.list_devices())


def wait_for_frames(cam: uc2_cam_py.Camera, n: int, timeout: float = 5.0) -> None:
    """Block until the current session holds at least ``n`` frames."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        img = cam.get_last_image()
        if img is not None and img.sequence + 1 >= n:
            return
        time.sleep(0.005)
    raise TimeoutError(f"no {n} frames within {timeout} s")


def display_ready() -> bool:
    return display_available() and bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if display_ready():
        return
    skip = pytest.mark.skip(reason="no display (DISPLAY/WAYLAND_DISPLAY) or no X11 build")
    for item in items:
        if "display" in item.keywords:
            item.add_marker(skip)


@pytest.fixture
def rec_logger() -> RecordingLogger:
    return RecordingLogger()


@pytest.fixture
def recording_dir(tmp_path: Path) -> Path:
    return tmp_path / "recordings"


def _open(vendor: str, logger: RecordingLogger, recording_dir: Path, **kwargs) -> uc2_cam_py.Camera:
    options = dict(SIM_OPTIONS) if vendor == "Simulated" else {}
    options.update(kwargs)
    options.setdefault("buffer_size", BUFFER)
    options.setdefault("store_image_data", False)
    options.setdefault("image_file_path", recording_dir)
    return uc2_cam_py.create_camera(vendor=vendor, logger=logger, **options)


@pytest.fixture
def open_camera(rec_logger: RecordingLogger, recording_dir: Path):
    """Factory ``open_camera(vendor="Simulated", **create_camera_kwargs)``; closes all at the end."""
    opened: list[uc2_cam_py.Camera] = []

    def factory(vendor: str = "Simulated", **kwargs) -> uc2_cam_py.Camera:
        if vendor == "HikRobot" and not hik_camera_connected():
            pytest.skip("no HikRobot camera connected")
        cam = _open(vendor, rec_logger, recording_dir, **kwargs)
        opened.append(cam)
        return cam

    yield factory
    for cam in opened:
        cam.close()


@pytest.fixture
def sim_camera(open_camera) -> uc2_cam_py.Camera:
    return open_camera("Simulated")


@pytest.fixture(params=["Simulated", pytest.param("HikRobot", marks=pytest.mark.hardware)])
def camera(request, open_camera) -> Iterator[uc2_cam_py.Camera]:
    """The interface tests' camera: once simulated, once a real HikRobot camera."""
    yield open_camera(request.param)
