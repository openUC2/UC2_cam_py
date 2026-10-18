"""RAM and disk-space queries used for the buffer and recording checks.

Separate module so tests can replace them (``monkeypatch.setattr``).
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Optional


def available_memory() -> Optional[int]:
    """Bytes of RAM available to new allocations, or None if unknown.

    Linux: ``MemAvailable`` from ``/proc/meminfo``; elsewhere free physical
    pages via ``os.sysconf`` where the platform offers it.
    """
    try:
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    try:
        return int(os.sysconf("SC_AVPHYS_PAGES")) * int(os.sysconf("SC_PAGE_SIZE"))
    except (AttributeError, ValueError, OSError):
        return None


def free_disk(path: Path) -> Optional[int]:
    """Free bytes on the file system holding ``path`` (or its nearest existing parent)."""
    p = Path(path).expanduser().absolute()
    while not p.exists() and p != p.parent:
        p = p.parent
    try:
        return int(shutil.disk_usage(p).free)
    except OSError:
        return None
