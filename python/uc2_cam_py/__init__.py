"""uc2_cam_py - camera interface for the UC2 ecosystem.

Open a camera with :func:`create_camera`; it returns a :class:`Camera` with a
ring buffer, optional recording to disk and zero-copy image access::

    import uc2_cam_py

    with uc2_cam_py.create_camera(vendor="HikRobot", start_on_init=True) as cam:
        img = cam.get_last_image()          # ImgData | None
        if img is not None:
            print(img.data.shape, img.time_ns)

Vendors: "HikRobot" (MVS SDK) and "Simulated" (synthetic images, no hardware).
The interface specification is ``Docs/UC2_cam_py_interface.md``.

Importing the package initialises the MVS SDK (finalised at interpreter exit)
and starts forwarding the C++ core's log messages to Python ``logging``.
"""

from __future__ import annotations

import atexit
import warnings

# First of all: the SDK reads part of its environment while the extension loads.
from . import _sdk_env  # noqa: F401

# isort: split
from . import _hik, _logging, defaults
from ._hik import DeviceInfo, finalize, initialize
from .camera import (
    BINNING_FACTORS,
    CONNECTION_TYPES,
    Camera,
    CameraInfo,
    create_camera,
    list_cameras,
    vendors,
)
from .data import ImgData, ImgMetaData
from .hikrobot import HikRobotCamera
from .simulated import SimulatedCamera

__version__ = "0.2.0"

__all__ = [
    "create_camera",
    "list_cameras",
    "CameraInfo",
    "Camera",
    "ImgData",
    "ImgMetaData",
    "HikRobotCamera",
    "SimulatedCamera",
    "DeviceInfo",
    "list_devices",
    "run_connection_test",
    "vendors",
    "CONNECTION_TYPES",
    "BINNING_FACTORS",
    "defaults",
    "initialize",
    "finalize",
    "__version__",
]


def list_devices(connection_type: str | None = None) -> list[DeviceInfo]:
    """List the connected HikRobot cameras (e.g. to find serial numbers).

    Args:
        connection_type: ``"USB3"``, ``"GigE"`` or None for both.

    Returns:
        list[DeviceInfo]: One entry per camera, in enumeration (= index) order.
    """
    if connection_type is not None and connection_type not in CONNECTION_TYPES:
        raise ValueError(f"connection_type must be one of {CONNECTION_TYPES} or None")
    return _hik.list_devices(connection_type or "")


def run_connection_test(index: int = -1, frames: int = 0, snapshot: str = "") -> int:
    """Quick connection test of a HikRobot camera: open it and grab frames.

    A diagnostic tool, not the acquisition API. It opens no window - the only
    live view is ``create_camera(..., debug_window=True)``.

    Args:
        index: Device index, or -1 to prompt on the terminal.
        frames: Stop after N frames; 0 = until Enter.
        snapshot: Path for the first-frame snapshot (.bmp/.jpg/.png/.tif);
            empty -> ``snapshot.png`` in the current directory.

    Returns:
        int: 0 on success, 2 if no camera was found, 1 on other errors.
    """
    return _hik.run_connection_test(index=index, frames=frames, snapshot=snapshot, display=False)


# Forward C++ log messages before the SDK is initialised, so that its messages
# already reach Python logging.
_logging.start()

if not initialize():
    warnings.warn(
        "MVS SDK initialisation failed; HikRobot cameras will not work.",
        RuntimeWarning,
        stacklevel=2,
    )
else:
    atexit.register(finalize)
