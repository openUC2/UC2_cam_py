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

import pytest
import uc2_cam_py
import logging

def test_import_and_version() -> None:
    """The package imports and exposes the expected public API."""
    assert uc2_cam_py.__version__ == "0.2.0"
    assert callable(uc2_cam_py.list_devices)
    assert callable(uc2_cam_py.run_connection_test)
    assert uc2_cam_py.__file__  # loaded from an installed location


def test_list_devices_returns_list() -> None:
    """list_devices() always returns a list (empty when no camera is present)."""
    devices = uc2_cam_py.list_devices()
    assert isinstance(devices, list)


@pytest.mark.hardware
def test_run_connection_test() -> None:
    """End-to-end connection test on the first device. Skipped without hardware."""
    devices = uc2_cam_py.list_devices()
    if not devices:
        pytest.skip("no HikRobot/MVS camera connected")
    rc = uc2_cam_py.run_connection_test(index=0, frames=10, snapshot="snapshot.png", display=False)
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

    # Grab a few frames, show the preview window (if available) and save the
    # first frame to snapshot.png.
    return uc2_cam_py.run_connection_test(index=0, frames=50, snapshot="snapshot.png", display=True)

def setLogger() -> None:
    """Set up logging for the smoke test.
        This function should just be copied as it is for preparing the logging configuration for later.
    
    """

    import logging.config
    import os;
    LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
    if not os.path.exists(LOG_DIR):
        os.makedirs(LOG_DIR)

    logging.config.dictConfig({
        "version": 1,
        "diable_existing_loggers": False, 
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
