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

from . import _hik, _logging, defaults
from ._hik import DeviceInfo, finalize, initialize, run_connection_test
from .camera import BINNING_FACTORS, CONNECTION_TYPES, Camera, create_camera, vendors
from .data import ImgData, ImgMetaData
from .hikrobot import HikRobotCamera
from .simulated import SimulatedCamera

__version__ = "0.2.0"

__all__ = [
    "create_camera",
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


def list_devices(connection_type: "str | None" = None) -> "list[DeviceInfo]":
    """List the connected HikRobot cameras (e.g. to find serial numbers).

    Args:
        connection_type: ``"USB3"``, ``"GigE"`` or None for both.

    Returns:
        list[DeviceInfo]: One entry per camera, in enumeration (= index) order.
    """
    if connection_type is not None and connection_type not in CONNECTION_TYPES:
        raise ValueError(f"connection_type must be one of {CONNECTION_TYPES} or None")
    return _hik.list_devices(connection_type or "")


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
