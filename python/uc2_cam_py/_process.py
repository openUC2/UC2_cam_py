"""Cameras in their own process - the default of ``create_camera(..., separate_process=True)``.

Each camera then runs in a process of its own (the program ``uc2_cam_worker``,
installed next to the extension module): device, acquisition thread and disk
writer, all SDK threads included. This process only sends settings over a
socket; the images are read zero-copy from the ring buffer in shared memory,
as with a camera in this process. The kernel spreads the camera processes over
the CPU cores, and a crash of a camera's SDK ends only its own process.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

from . import _hik

#: File name of the camera process program.
WORKER_NAME = "uc2_cam_worker"


def worker_path() -> Path:
    """The camera process program installed next to the extension module.

    Raises:
        RuntimeError: Not on Linux, or the program is missing.
    """
    if not sys.platform.startswith("linux"):
        raise RuntimeError("separate_process=True is only available on Linux; use separate_process=False")
    path = Path(_hik.__file__).parent / WORKER_NAME
    if not path.is_file():
        raise RuntimeError(
            f"the camera process program {path} is missing - reinstall uc2_cam_py, or use separate_process=False"
        )
    return path


def _text(value: Any) -> str:
    # repr() keeps every digit of a float; C++ reads it back with strtod.
    return repr(value) if isinstance(value, float) else str(value)


def open_camera(
    vendor: str, connection_type: str | None, serial: str | None, index: int | None, options: dict[str, Any]
) -> Any:
    """Start a camera process and open the camera in it.

    Raises:
        ValueError, IndexError, RuntimeError: As the in-process backends do.
    """
    return _hik.RemoteCamera(
        str(worker_path()),
        vendor,
        connection_type or "",
        serial or "",
        -1 if index is None else index,
        {key: _text(value) for key, value in options.items()},
    )
