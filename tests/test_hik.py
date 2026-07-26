"""Pytest suite for the uc2_hikpy Python bindings.

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
import uc2_hikpy


def test_import_and_version() -> None:
    """The package imports and exposes the expected public API."""
    assert uc2_hikpy.__version__ == "0.1.0"
    assert callable(uc2_hikpy.list_devices)
    assert callable(uc2_hikpy.run)
    assert uc2_hikpy.__file__  # loaded from an installed location


def test_list_devices_returns_list() -> None:
    """list_devices() always returns a list (empty when no camera is present)."""
    devices = uc2_hikpy.list_devices()
    assert isinstance(devices, list)


@pytest.mark.hardware
def test_run_capture() -> None:
    """End-to-end capture on the first device. Skipped without hardware."""
    devices = uc2_hikpy.list_devices()
    if not devices:
        pytest.skip("no HikRobot/MVS camera connected")
    rc = uc2_hikpy.run(index=0, frames=10, snapshot="snapshot.png", display=False)
    assert rc == 0


def main() -> int:
    """Manual smoke run: list cameras and, if any, grab a few frames."""
    print(f"uc2_hikpy {uc2_hikpy.__version__}")
    print(f"Got file from {uc2_hikpy.__file__}")
    devices = uc2_hikpy.list_devices()
    print(f"Found {len(devices)} camera(s):")
    for i, dev in enumerate(devices):
        print(f"  [{i}] {dev!r}")

    if not devices:
        print("No cameras connected - nothing to start.")
        return 0

    # Grab a few frames, show the preview window (if available) and save the
    # first frame to snapshot.png.
    return uc2_hikpy.run(index=0, frames=50, snapshot="snapshot.png", display=True)


if __name__ == "__main__":
    raise SystemExit(main())
