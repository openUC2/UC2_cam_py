"""Reader for ``.uc2rb`` recordings (the ring buffer's container files).

Layout (``Docs/RingBuff_REF.md`` §8)::

    [UC2_SessionHeader  8448 B]
    [UC2_FrameHeader      64 B][payload: nbytes]   sequence n
    [UC2_FrameHeader      64 B][payload: nbytes]   sequence n+1
    ...

The numpy types below mirror the C++ structs of ``core/include/UC2_RingBuff.h``
byte for byte (``#pragma pack(8)``, little-endian); ``tests/test_exporter.py``
checks them against a header written by the C++ side. Used by
:mod:`uc2_cam_py.exporter`; not part of the public interface.
"""

from __future__ import annotations

import io
import os
from dataclasses import asdict, dataclass

import numpy as np

from . import _logging
from .data import ImgMetaData

#: ``UC2_SessionHeader`` (8448 B); fields in C++ order, with their offsets.
SESSION_HEADER = np.dtype({
    "names": ["magic", "formatVersion", "headerBytes", "width", "height", "channels", "depth",
              "frameBytes", "slotCount", "layoutVersion", "startWallNs", "startMonoNs", "slotStride",
              "cameraId", "comment", "softwareVersion"],
    "formats": ["S8", "<u4", "<u4", "<u4", "<u4", "<u4", "u1",
                "<u8", "<u4", "<u4", "<i8", "<i8", "<u8",
                "S64", "S8192", "S32"],
    "offsets": [0, 8, 12, 16, 20, 24, 28,
                32, 40, 44, 48, 56, 64,
                72, 136, 8328],
    "itemsize": 8448,
})

#: ``UC2_FrameHeader`` (64 B), in front of every frame.
FRAME_HEADER = np.dtype({
    "names": ["sequence", "nbytes", "tsBeforeNs", "tsAfterNs", "slot", "flags",
              "width", "height", "channels", "depth"],
    "formats": ["<u8", "<u8", "<i8", "<i8", "<u4", "<u4", "<u4", "<u4", "<u4", "u1"],
    "offsets": [0, 8, 16, 24, 32, 36, 40, 44, 48, 52],
    "itemsize": 64,
})

MAGIC = b"UC2RB"
FORMAT_VERSION = 1


@dataclass(frozen=True)
class FrameRecord:
    """One frame of a recording: its ``UC2_FrameHeader`` and where its pixels are."""

    sequence: int        #: global frame index of the session (may have gaps)
    nbytes: int          #: payload bytes
    time_before_ns: int  #: monotonic ns before the frame was read
    time_after_ns: int   #: monotonic ns after the frame was read
    slot: int            #: ring slot the frame occupied
    flags: int           #: FrameFlag bits when it was saved
    width: int
    height: int
    channels: int
    depth: int           #: bytes per sample (PixelDepth)
    offset: int          #: file offset of the payload

    @property
    def time_ns(self) -> int:
        """Midpoint of the two timestamps, as ``ImgData.time_ns``."""
        return (self.time_before_ns + self.time_after_ns) // 2

    def header(self) -> dict[str, int]:
        """The frame header fields (without the file offset)."""
        d = asdict(self)
        del d["offset"]
        return d


def _text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def read_session_header(f: io.BufferedReader, path: str | os.PathLike) -> ImgMetaData:
    """Read and check the session header at the start of ``f``.

    Raises:
        ValueError: Not a ``.uc2rb`` file, or a layout this reader does not know.
    """
    raw = f.read(SESSION_HEADER.itemsize)
    if len(raw) < len(MAGIC) or not raw.startswith(MAGIC):
        raise ValueError(f"{path} is not a uc2rb recording (no UC2RB header)")
    if len(raw) < SESSION_HEADER.itemsize:
        raise ValueError(f"{path}: the session header is cut off ({len(raw)} of {SESSION_HEADER.itemsize} bytes)")
    h = np.frombuffer(raw, SESSION_HEADER)[0]
    if int(h["formatVersion"]) != FORMAT_VERSION:
        raise ValueError(f"{path}: container format version {int(h['formatVersion'])} is not supported "
                         f"(this reader knows version {FORMAT_VERSION})")
    if int(h["headerBytes"]) != SESSION_HEADER.itemsize:
        raise ValueError(f"{path}: a session header of {int(h['headerBytes'])} bytes is not supported "
                         f"(this reader knows {SESSION_HEADER.itemsize}; files written before the 8 KiB "
                         "comment have another layout)")
    return ImgMetaData(
        magic=_text(bytes(h["magic"])),
        format_version=int(h["formatVersion"]),
        header_bytes=int(h["headerBytes"]),
        width=int(h["width"]),
        height=int(h["height"]),
        channels=int(h["channels"]),
        depth=int(h["depth"]),
        frame_bytes=int(h["frameBytes"]),
        slot_count=int(h["slotCount"]),
        layout_version=int(h["layoutVersion"]),
        start_wall_ns=int(h["startWallNs"]),
        start_mono_ns=int(h["startMonoNs"]),
        slot_stride=int(h["slotStride"]),
        camera_id=_text(bytes(h["cameraId"])),
        comment=_text(bytes(h["comment"])),
        software_version=_text(bytes(h["softwareVersion"])),
    )


def frame_records(f: io.BufferedReader, path: str | os.PathLike, meta: ImgMetaData) -> list[FrameRecord]:
    """All complete frames of the recording, read header by header (the pixels are skipped).

    A cut-off last record - the recording was interrupted, or is still being written - is
    left out with a warning.

    Raises:
        ValueError: A frame whose geometry differs from the session header.
    """
    size = os.fstat(f.fileno()).st_size
    expected = meta.width * meta.height * meta.channels * meta.depth
    records: list[FrameRecord] = []
    pos = meta.header_bytes
    while pos < size:
        if pos + FRAME_HEADER.itemsize > size:
            _logging.package_logger().warning(f"{path}: the last frame header is cut off; it is left out")
            break
        f.seek(pos)
        h = np.frombuffer(f.read(FRAME_HEADER.itemsize), FRAME_HEADER)[0]
        rec = FrameRecord(
            sequence=int(h["sequence"]),
            nbytes=int(h["nbytes"]),
            time_before_ns=int(h["tsBeforeNs"]),
            time_after_ns=int(h["tsAfterNs"]),
            slot=int(h["slot"]),
            flags=int(h["flags"]),
            width=int(h["width"]),
            height=int(h["height"]),
            channels=int(h["channels"]),
            depth=int(h["depth"]),
            offset=pos + FRAME_HEADER.itemsize,
        )
        geometry = (rec.width, rec.height, rec.channels, rec.depth, rec.nbytes)
        if geometry != (meta.width, meta.height, meta.channels, meta.depth, expected):
            raise ValueError(f"{path}: frame {rec.sequence} (width, height, channels, depth, bytes) = {geometry} "
                             "does not match the session header")
        if rec.offset + rec.nbytes > size:
            _logging.package_logger().warning(
                f"{path}: frame {rec.sequence} is cut off ({size - rec.offset} of {rec.nbytes} bytes); it is left out"
            )
            break
        records.append(rec)
        pos = rec.offset + rec.nbytes
    return records


def read_frame(f: io.BufferedReader, rec: FrameRecord, meta: ImgMetaData) -> np.ndarray:
    """The pixels of one frame as a new ``(H, W, C)`` array."""
    frame = np.empty((rec.height, rec.width, rec.channels), dtype=meta.dtype)
    f.seek(rec.offset)
    if f.readinto(memoryview(frame).cast("B")) != rec.nbytes:
        raise ValueError(f"frame {rec.sequence}: the file ended inside its pixels")
    return frame
