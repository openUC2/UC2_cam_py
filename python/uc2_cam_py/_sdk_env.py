"""Prepare the process environment for the MVS SDK - before the extension loads it.

Imported first by the package. The SDK reads ``ALLUSERSPROFILE`` (the parent of
its GenICam XML cache) once, while libMvCameraControl loads, so it has to be set
before ``_hik`` is imported; without it the SDK uses ``/GenICam/xml/cache``. An
MVS installer exports these variables system-wide pointing into /opt/MVS, so
they are replaced: uc2_cam_py uses the SDK copy in its own package (``_mvs/``)
and keeps the cache in the user's cache directory. Only this process is affected.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path


def cache_dir() -> Path:
    """Directory that holds the SDK's GenICam XML cache."""
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return Path(base) / "uc2_cam_py"


def bundled_sdk() -> Path | None:
    """The SDK copy installed next to the extension module, if there is one."""
    spec = importlib.util.find_spec("uc2_cam_py._hik")   # locates the module without loading it
    if spec is None or spec.origin is None:
        return None
    root = Path(spec.origin).parent / "_mvs"
    return root if root.is_dir() else None


def prepare() -> None:
    cache = cache_dir()
    try:
        cache.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass   # the SDK then just runs without its XML cache
    os.environ["ALLUSERSPROFILE"] = str(cache)
    root = bundled_sdk()
    if root is not None:
        os.environ["MVCAM_SDK_PATH"] = str(root)
        os.environ["MVCAM_COMMON_RUNENV"] = str(root / "lib")
        os.environ["MVCAM_GENICAM_CLPROTOCOL"] = str(root / "lib" / "CLProtocol")


prepare()
