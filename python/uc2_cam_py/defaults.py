"""Default values of uc2_cam_py.

Kept in one place so a later ``config.yaml`` can override them without touching
the camera code. Every value here is read at the time it is needed (not
captured at import), so assigning e.g. ``defaults.BUFFER_SIZE = 32`` at runtime
takes effect for cameras created afterwards.
"""

from __future__ import annotations

from pathlib import Path

#: Vendor used by :func:`uc2_cam_py.create_camera` when none is given.
VENDOR: str = "HikRobot"

#: Number of images (slots) the ring buffer holds.
BUFFER_SIZE: int = 16

#: A buffer larger than this many bytes is reported with a warning.
BUFFER_WARN_BYTES: int = 2 * 1024**3

#: Directory for recordings when ``image_file_path`` is None.
IMAGE_FILE_PATH: Path = Path.home() / "UC2_recordings"

#: Name of the package logger (used when no logger is passed).
LOGGER_NAME: str = "uc2_cam_py"
