"""Pytest suite for the uc2_cam_py Python bindings.

The unit tests here need no camera: importing the package auto-initialises the
MVS SDK but degrades gracefully (a RuntimeWarning, not a crash) when no device
is attached, and ``list_devices()`` simply returns an empty list.

Run the unit tests with:      uv run pytest
Run the camera test too with:  uv run pytest -m hardware   # needs a camera

The module also stays runnable as a manual smoke script:
    uv run python tests/test_hik.py
"""

from __future__ import annotations

import logging

import pytest

import uc2_cam_py

#: Tests of the bundled MVS SDK; a build without it has none.
needs_mvs = pytest.mark.skipif(not uc2_cam_py._hik.HAVE_MVS, reason="built without the MVS SDK")


def test_import_and_version() -> None:
    """The package imports and exposes the expected public API; one version everywhere."""
    import importlib.metadata
    import re
    from pathlib import Path

    from uc2_cam_py import _hik

    version = (Path(__file__).parents[1] / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)              # SemVer MAJOR.MINOR.PATCH
    assert uc2_cam_py.__version__ == version
    assert _hik.__version__ == version                            # compiled into the C++ core
    assert importlib.metadata.version("uc2_cam_py") == version    # package metadata
    assert callable(uc2_cam_py.list_devices)
    assert callable(uc2_cam_py.run_connection_test)
    assert uc2_cam_py.__file__  # loaded from an installed location


def test_the_camera_process_reports_the_same_version() -> None:
    """``uc2_cam_worker --version`` names the version of the package it was installed with."""
    import subprocess

    from uc2_cam_py import _process

    out = subprocess.run([_process.worker_path(), "--version"], capture_output=True, text=True,
                         check=True, timeout=10)
    assert out.stdout.strip() == f"uc2_cam_worker {uc2_cam_py.__version__}"


@pytest.mark.skipif(uc2_cam_py._hik.HAVE_MVS, reason="only in a build without the MVS SDK")
def test_a_build_without_the_sdk_says_so_and_offers_only_the_simulated_camera() -> None:
    """No MVS SDK at build time: importing logs why HikRobot is missing, and nothing of it works."""
    import subprocess
    import sys

    out = subprocess.run([sys.executable, "-c", "import uc2_cam_py"], capture_output=True, text=True,
                         check=True, timeout=60)
    assert "HikRobot cameras are not available" in out.stderr     # logging.lastResort -> stderr
    assert uc2_cam_py._hik.MVS_MISSING_REASON                     # why the build has no SDK ...
    assert uc2_cam_py._hik.MVS_MISSING_REASON in out.stderr       # ... is part of the message
    assert "uv run fetch_mvs" in out.stderr                       # and how to add them
    assert uc2_cam_py.vendors() == ["Simulated"]
    assert uc2_cam_py.list_devices() == []
    assert uc2_cam_py._hik.sdk_path() == ""
    assert not hasattr(uc2_cam_py._hik, "HikCamera")
    with pytest.raises(RuntimeError, match="built without the MVS SDK"):
        uc2_cam_py.run_connection_test(frames=1)


def test_list_devices_returns_list() -> None:
    """list_devices() always returns a list (empty when no camera is present)."""
    devices = uc2_cam_py.list_devices()
    assert isinstance(devices, list)


@needs_mvs
def test_a_build_with_the_sdk_has_no_missing_reason() -> None:
    assert uc2_cam_py._hik.MVS_MISSING_REASON == ""


@needs_mvs
def test_the_sdk_comes_from_the_package_not_from_an_installation() -> None:
    """The MVS SDK is the copy in uc2_cam_py/_mvs; nothing is loaded from /opt/MVS."""
    import os
    from pathlib import Path

    from uc2_cam_py import _hik

    uc2_cam_py.list_devices()                 # loads the transport layer plug-ins as well
    root = Path(_hik.sdk_path())
    assert root == Path(_hik.__file__).parent / "_mvs"
    assert (root / "VERSION").is_file()
    with open("/proc/self/maps") as maps:
        files = {line.split()[-1] for line in maps if "/" in line}
    assert not [f for f in files if f.startswith("/opt/MVS")]
    assert any(f.startswith(str(root)) for f in files)
    assert os.environ["ALLUSERSPROFILE"].endswith("uc2_cam_py")    # GenICam cache not in /opt/MVS


@needs_mvs
def test_the_camera_process_loads_the_sdk_of_the_package_too(tmp_path) -> None:
    """The camera process (separate_process=True) uses the same SDK copy, never /opt/MVS."""
    from pathlib import Path

    from uc2_cam_py import _hik

    root = Path(_hik.sdk_path())
    with uc2_cam_py.create_camera(
        vendor="Simulated", store_image_data=False, image_file_path=tmp_path, sensor_width=400, sensor_height=200
    ) as cam:
        assert cam.process_id is not None
        with open(f"/proc/{cam.process_id}/maps") as maps:
            files = {line.split()[-1] for line in maps if "/" in line}
        with open(f"/proc/{cam.process_id}/environ", "rb") as env:
            environ = env.read().split(b"\0")
    assert not [f for f in files if f.startswith("/opt/MVS")]
    assert any(f.startswith(str(root)) for f in files)
    assert any(e.startswith(b"ALLUSERSPROFILE=") and e.endswith(b"uc2_cam_py") for e in environ)


@pytest.mark.hardware
def test_run_connection_test() -> None:
    """End-to-end connection test on the first device. Skipped without hardware."""
    devices = uc2_cam_py.list_devices()
    if not devices:
        pytest.skip("no HikRobot/MVS camera connected")
    rc = uc2_cam_py.run_connection_test(index=0, frames=10, snapshot="snapshot.png")
    assert rc == 0


def main() -> int:
    """Manual smoke run: list cameras and, if any, grab a few frames."""
    log.info(f"uc2_cam_py {uc2_cam_py.__version__}")
    
    log.info(f"Got file from {uc2_cam_py.__file__}")
    devices = uc2_cam_py.list_devices()
    log.info(f"Found {len(devices)} camera(s):")
    for i, dev in enumerate(devices):
        log.info(f"  [{i}] {dev!r}")

    if not devices:
        log.info("No cameras connected - nothing to start.")
        return 0

    # Grab a few frames and save the first one to snapshot.png.
    return uc2_cam_py.run_connection_test(index=0, frames=50, snapshot="snapshot.png")

def setLogger() -> None:
    """Set up logging for the smoke test.
        This function should just be copied as it is for preparing the logging configuration for later.
    
    """

    import logging.config
    import os
    LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
    if not os.path.exists(LOG_DIR):
        os.makedirs(LOG_DIR)

    logging.config.dictConfig({
        "version": 1,
        "disable_existing_loggers": False,
        "formatters": {"std": {"format": "%(asctime)s %(levelname)-8s %(name)s: %(message)s"}},
        "handlers": {
            "console": {"class": "logging.StreamHandler", "formatter": "std"},
            "file": {"class": "logging.handlers.RotatingFileHandler", 
                     "formatter": "std",
                     "filename": os.path.join(LOG_DIR, "app.log"), 
                     "maxBytes": 10_000_000, 
                     "backupCount": 5},
        },
        "root": {"level": "DEBUG", "handlers": ["console", "file"]},
    })
    

if __name__ == "__main__":
    setLogger()
    log = logging.getLogger(__name__)
    log.info("Starting uc2_cam_py smoke test...")
    raise SystemExit(main())
