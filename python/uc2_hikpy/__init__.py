"""uc2_hikpy - Python bindings for the MVS/HikRobot camera test tool.

Importing this package initialises the MVS SDK and registers finalisation at
interpreter exit, so you can call :func:`list_devices`,
:func:`run_connection_test` and build a :class:`Grabber` directly.
"""

from __future__ import annotations

import atexit
import warnings

from ._hik import (
    AutoMode,
    DeviceInfo,
    Frame,
    FrameGrabber,
    Roi,
    finalize,
    initialize,
    list_devices,
    run_connection_test,
)
from .grabber import Grabber

__version__ = "0.1.0"

__all__ = [
    "AutoMode",
    "DeviceInfo",
    "Frame",
    "FrameGrabber",
    "Roi",
    "Grabber",
    "list_devices",
    "run_connection_test",
    "initialize",
    "finalize",
    "__version__",
]

# Initialise the SDK once when the package is imported and make sure it is
# finalised when the interpreter shuts down. initialize() also preloads the MVS
# renderer so the live preview works without LD_LIBRARY_PATH being exported.
if not initialize():
    warnings.warn(
        "MVS SDK initialisation failed; camera functions may not work.",
        RuntimeWarning,
        stacklevel=2,
    )
else:
    atexit.register(finalize)
