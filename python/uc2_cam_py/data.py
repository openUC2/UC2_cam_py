"""Public data classes of uc2_cam_py: :class:`ImgData` and :class:`ImgMetaData`.

Both are plain ``@dataclass`` types, importable as
``from uc2_cam_py import ImgData, ImgMetaData`` and usable for type hints,
``isinstance`` checks and ``dataclasses.asdict``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import numpy as np


@dataclass(eq=False)
class ImgData:
    """One image from the camera's ring buffer.

    ``data`` is a **zero-copy, read-only** numpy view onto the ring slot, always
    three-dimensional: shape ``(H, W, C)`` with C = 1 (mono, raw Bayer) or 3
    (RGB/BGR), dtype ``uint8`` or ``uint16``. ``data[..., 0]`` is the 2D view of
    a single channel, again without a copy. The view keeps the buffer memory
    alive, also after the camera was closed. In the default overwrite mode the
    camera may recycle the slot while you are still reading it - call
    :meth:`is_valid` *after* processing to know whether what you read was
    consistent. Use ``data.copy()`` to keep an image independently of the buffer.

    Timestamps are monotonic nanoseconds (``time.monotonic_ns()`` clock):

    Attributes:
        data: Pixel data (read-only view onto the ring slot).
        time_before_ns: When the frame reached the host (camera callback).
        time_after_ns: When the copy into the ring slot was complete.
        time_ns: Midpoint ``(time_before_ns + time_after_ns) // 2``.
        sequence: Global frame index within the recording session (0, 1, ...).
        slot: Ring-buffer slot (``sequence % buffer_size``).
    """

    data: np.ndarray
    time_before_ns: int
    time_after_ns: int
    time_ns: int
    sequence: int
    slot: int
    _handle: Any = field(default=None, repr=False, compare=False)

    def is_valid(self) -> bool:
        """Check whether the camera has not overwritten the slot since it was read.

        Returns:
            bool: True if ``data`` still holds exactly this frame. Always True
            for an ``ImgData`` that was not read from a camera.
        """
        return True if self._handle is None else bool(self._handle.still_valid())

    @classmethod
    def _from_handle(cls, handle: Any) -> ImgData:
        before = int(handle.ts_before_ns)
        after = int(handle.ts_after_ns)
        return cls(
            data=handle.array,
            time_before_ns=before,
            time_after_ns=after,
            time_ns=(before + after) // 2,
            sequence=int(handle.sequence),
            slot=int(handle.slot),
            _handle=handle,
        )


_DTYPE_BY_DEPTH = {1: "uint8", 2: "uint16", 8: "float64"}


@dataclass(frozen=True)
class ImgMetaData:
    """Metadata of a recording session: all fields of ``UC2_SessionHeader``.

    The same header starts every ``.uc2rb`` file on disk.

    Attributes:
        magic: File magic, ``"UC2RB"``.
        format_version: Version of the container format.
        header_bytes: Size of the session header in bytes.
        width: Image width in pixels (after ROI and binning).
        height: Image height in pixels.
        channels: Channels per pixel (1 or 3).
        depth: Bytes per sample (1 = uint8, 2 = uint16).
        frame_bytes: Bytes of one image (width * height * channels * depth).
        slot_count: Number of ring-buffer slots.
        layout_version: Version of the shared-memory layout.
        start_wall_ns: UTC epoch nanoseconds at the session start.
        start_mono_ns: Monotonic nanoseconds at the same instant (anchor for
            converting ``ImgData`` times to absolute time).
        slot_stride: Payload stride between slots in bytes.
        camera_id: ``"<model>-<serial>"``.
        comment: JSON with the camera settings at the session start.
        software_version: Version of uc2_cam_py that wrote the session (``""`` in
            files written before 1.0.0).
    """

    magic: str
    format_version: int
    header_bytes: int
    width: int
    height: int
    channels: int
    depth: int
    frame_bytes: int
    slot_count: int
    layout_version: int
    start_wall_ns: int
    start_mono_ns: int
    slot_stride: int
    camera_id: str
    comment: str
    software_version: str

    @property
    def dtype(self) -> str:
        """str: numpy dtype name of the samples (``"uint8"``, ``"uint16"``)."""
        return _DTYPE_BY_DEPTH.get(self.depth, "uint8")

    def wall_time_ns(self, monotonic_ns: int) -> int:
        """Convert a monotonic timestamp of this session to UTC epoch nanoseconds.

        Args:
            monotonic_ns: e.g. ``ImgData.time_ns``.

        Returns:
            int: UTC nanoseconds since the Unix epoch.
        """
        return self.start_wall_ns + (monotonic_ns - self.start_mono_ns)

    @classmethod
    def _from_dict(cls, d: dict[str, Any] | None) -> ImgMetaData | None:
        return None if d is None else cls(**d)
