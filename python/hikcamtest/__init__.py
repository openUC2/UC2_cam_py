"""HikCamtest - Python bindings for the MVS/HikRobot camera test tool.

Importing this package initialises the MVS SDK and registers finalisation at
interpreter exit, so you can call :func:`list_devices` and :func:`run` directly.
"""

from __future__ import annotations

import atexit
import warnings

from ._hikcamtest import DeviceInfo, finalize, initialize, list_devices, run

__version__ = "0.1.0"

__all__ = [
    "DeviceInfo",
    "list_devices",
    "run",
    "initialize",
    "finalize",
    "__version__",
]

print(f"Imported _hikcamtest from {_hikcamtest.__file__}")

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
