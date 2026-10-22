"""Vendor-neutral camera interface of uc2_cam_py.

:func:`create_camera` opens a camera of the requested vendor and returns a
:class:`Camera`. The full specification (units, ``None`` semantics, logging,
errors) is in ``Docs/UC2_cam_py_interface.md``.

Units: exposure in milliseconds, frame rate in frames per second, gain in dB,
ROI ``(x, y, width, height)`` in unbinned sensor pixels.
"""

from __future__ import annotations

import errno
import json
import math
import numbers
import os
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import Any, ClassVar, Self

from . import _logging, _preview, _resources, defaults
from .data import ImgData, ImgMetaData

PathLike = str | os.PathLike[str]
Range = tuple[float, float]
Roi = tuple[int, int, int, int]

#: Connection types accepted by :func:`create_camera` (canonical spelling).
CONNECTION_TYPES: tuple[str, ...] = ("USB3", "GigE")

#: Binning factors of the interface specification.
BINNING_FACTORS: tuple[int, ...] = (1, 2, 4)

_VENDORS: dict[str, type[Camera]] = {}


def register_vendor(cls: type[Camera]) -> type[Camera]:
    """Class decorator: make ``cls`` available as ``create_camera(vendor=cls.VENDOR)``."""
    _VENDORS[cls.VENDOR.lower()] = cls
    return cls


def vendors() -> list[str]:
    """Names of the available vendors (e.g. ``["HikRobot", "Simulated"]``)."""
    return [c.VENDOR for c in _VENDORS.values()]


@dataclass(frozen=True)
class CameraInfo:
    """One camera found by :func:`list_cameras`.

    Open it with ``create_camera(vendor=info.vendor, serial=info.serial)``.

    Attributes:
        vendor (str): Vendor name as accepted by :func:`create_camera`.
        model (str): Camera model name.
        serial (str): Serial number.
        connection_type (str): ``"USB3"``, ``"GigE"``, or ``""`` for a simulated camera.
    """

    vendor: str
    model: str
    serial: str
    connection_type: str


def list_cameras() -> list[CameraInfo]:
    """All cameras that can be opened, over every vendor (in the order of :func:`vendors`).

    A vendor whose search fails (e.g. its SDK is not initialised) is skipped
    with a warning; the others are still listed.

    Returns:
        list[CameraInfo]: One entry per camera.
    """
    found: list[CameraInfo] = []
    for cls in _VENDORS.values():
        try:
            found.extend(cls._discover())
        except Exception as exc:  # noqa: BLE001 - one vendor must not hide the others
            _logging.package_logger().warning(f"Searching {cls.VENDOR} cameras failed: {exc}")
    return found


def _is_real(value: Any) -> bool:
    return isinstance(value, numbers.Real) and not isinstance(value, bool)


def _is_int(value: Any) -> bool:
    return isinstance(value, numbers.Integral) and not isinstance(value, bool)


class Camera:
    """A camera with a ring buffer, optional recording to disk and logging.

    Do not instantiate directly; use :func:`create_camera`. A camera is a
    context manager: ``with create_camera(...) as cam:`` closes it on exit.

    Thread safety: the methods may be called from several Python threads; the
    acquisition itself runs on a C++ thread and never needs the GIL.
    """

    #: Vendor name as accepted by :func:`create_camera`; set by subclasses.
    VENDOR: ClassVar[str] = ""

    # ------------------------------------------------------------------ setup

    def __init__(
        self,
        backend: Any,
        *,
        buffer_size: int,
        start_on_init: bool,
        image_file_path: PathLike | None,
        store_image_data: bool,
        logger: _logging.LoggerAdapter,
        debug_window: bool,
    ) -> None:
        self._backend = backend
        self._log = logger
        self._closed = False
        self._debug_window: _preview.DebugWindow | None = None
        self._buffer_size = int(buffer_size)
        self._store_image_data = bool(store_image_data)
        path = defaults.IMAGE_FILE_PATH if image_file_path is None else image_file_path
        self._image_file_path = Path(path).expanduser()
        _logging.set_active_logger(logger)

        sensor = backend.sensor_size()
        if sensor is None:
            info = self._model_info()
            sensor = None if info is None else info.get("pixel_count")
        if sensor is None:
            self._backend.close()
            message = (
                f"{self.VENDOR} {backend.model}: the pixel count cannot be read from the firmware "
                f"and is not coded in the camera class - add the model to its model table"
            )
            self._log.error(message)
            raise RuntimeError(message)
        self._pixel_count = (int(sensor[0]), int(sensor[1]))

        try:
            self._check_resources(log_disk_estimate=True)
        except Exception:
            self._backend.close()
            raise
        self._log_init_info()
        if start_on_init:
            self.start()
        if debug_window:
            self._debug_window = _preview.DebugWindow(
                self.get_last_image,
                self._backend.wait_for_frame,
                f"uc2_cam_py debug - {self.VENDOR} {self.model} SN {self.serial}",
                self._log,
            )

    @classmethod
    def _create_backend(
        cls, connection_type: str | None, serial: str | None, index: int | None, **options: Any
    ) -> Any:
        raise NotImplementedError

    @classmethod
    def _discover(cls) -> list[CameraInfo]:
        """The cameras of this vendor that can be opened now (for :func:`list_cameras`)."""
        return []

    def _model_info(self) -> dict[str, Any] | None:
        """Static model data (pixel_pitch_um, pixel_size_um, pixel_count) or None."""
        return None

    def _log_init_info(self) -> None:
        b = self._backend
        _, channels, itemsize, fmt = b.pixel_layout()
        size = b.image_size() or (0, 0)
        frame_bytes = size[0] * size[1] * channels * itemsize
        self._log.info(
            f"Camera initialised: {self.VENDOR} {b.model}, serial {b.serial}, "
            f"{b.connection_type}, index {b.index}"
        )
        self._log.info(
            f"Sensor {self._pixel_count[0]}x{self._pixel_count[1]} px, pitch "
            f"{self.pixel_pitch_um} um, image {size[0]}x{size[1]} {fmt}, binning {self.binning}"
        )
        self._log.info(
            f"Buffer {self._buffer_size} slots ({self._buffer_size * frame_bytes / 2**20:.1f} MiB), "
            + (f"recording to {self._image_file_path}" if self._store_image_data else "not recording to disk")
        )
        self._log.info(
            "Settable: "
            + ", ".join(
                f"{name}={getattr(self, 'can_set_' + name)}"
                for name in ("exposure", "gain", "frame_rate", "binning", "trigger")
            )
        )

    # -------------------------------------------------------------- resources

    def _frame_bytes(self) -> int:
        _, channels, itemsize, _ = self._backend.pixel_layout()
        size = self._backend.image_size() or (0, 0)
        return int(size[0]) * int(size[1]) * int(channels) * int(itemsize)

    def _check_resources(self, log_disk_estimate: bool) -> None:
        """Check RAM for the buffer and disk space for the recording.

        Raises:
            MemoryError: The buffer does not fit into the available RAM.
            OSError: The recording directory cannot be created, or the disk has
                less free space than one full buffer.
        """
        frame_bytes = self._frame_bytes()
        total = self._buffer_size * frame_bytes
        if total > defaults.BUFFER_WARN_BYTES:
            self._log.warning(
                f"Image buffer of {total / 2**30:.2f} GiB ({self._buffer_size} slots) exceeds the "
                f"warning threshold of {defaults.BUFFER_WARN_BYTES / 2**30:.2f} GiB"
            )
        available = _resources.available_memory()
        if available is not None and total > available:
            message = (
                f"Image buffer of {total / 2**20:.0f} MiB does not fit into the available RAM "
                f"({available / 2**20:.0f} MiB) - reduce buffer_size"
            )
            self._log.error(message)
            raise MemoryError(message)
        if not self._store_image_data:
            return

        try:
            self._image_file_path.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            self._log.error(f"Cannot create the image file path {self._image_file_path}: {exc}")
            raise
        free = _resources.free_disk(self._image_file_path)
        if free is None:
            return
        if free < total:
            message = (
                f"Not enough disk space in {self._image_file_path}: {free / 2**20:.0f} MiB free, "
                f"at least one full buffer ({total / 2**20:.0f} MiB) is needed"
            )
            self._log.error(message)
            raise OSError(errno.ENOSPC, message)
        fps = self._backend.get_frame_rate()
        if log_disk_estimate and fps and frame_bytes:
            seconds = free / (frame_bytes * fps)
            self._log.info(
                f"Disk space for about {seconds / 60:.1f} min of recording at {fps:.3g} fps "
                f"({free / 2**30:.1f} GiB free)"
            )

    def _require_open(self) -> None:
        if self._closed:
            raise RuntimeError("the camera has been closed")

    # ------------------------------------------------------------- properties

    @property
    def can_set_gain(self) -> bool:
        """bool: Whether the gain can be set in the firmware."""
        return bool(self._backend.can_set_gain())

    @property
    def can_set_frame_rate(self) -> bool:
        """bool: Whether the frame rate can be set in the firmware."""
        return bool(self._backend.can_set_frame_rate())

    @property
    def can_set_binning(self) -> bool:
        """bool: Whether binning can be set in the firmware."""
        return bool(self._backend.can_set_binning())

    @property
    def can_set_trigger(self) -> bool:
        """bool: Whether the camera has a configurable trigger (not used yet)."""
        return bool(self._backend.can_set_trigger())

    @property
    def can_set_exposure(self) -> bool:
        """bool: Whether the exposure time can be set in the firmware."""
        return bool(self._backend.can_set_exposure())

    @property
    def can_set_roi(self) -> bool:
        """bool: Whether a region of interest can be set (addition to the spec)."""
        return bool(self._backend.can_set_roi())

    @property
    def range_exposure_time_ms(self) -> Range | None:
        """tuple[float, float] | None: (min, max) exposure time in ms; None if not settable."""
        r = self._backend.exposure_range_ms()
        return None if r is None else (max(0.0, float(r[0])), float(r[1]))

    @property
    def range_frame_rate_per_sec(self) -> Range | None:
        """tuple[float, float] | None: (min, max) frame rate in 1/s; None if not settable."""
        r = self._backend.frame_rate_range()
        return None if r is None else (max(0.0, float(r[0])), float(r[1]))

    @property
    def range_gain_db(self) -> Range | None:
        """tuple[float, float] | None: (min, max) gain in dB; None if not settable."""
        r = self._backend.gain_range_db()
        return None if r is None else (max(0.0, float(r[0])), float(r[1]))

    @property
    def range_binning(self) -> list[int]:
        """list[int]: Allowed symmetric binning factors (subset of 1, 2, 4); [1] if fixed."""
        if not self.can_set_binning:
            return [1]
        values = [int(b) for b in self._backend.binning_values() if int(b) in BINNING_FACTORS]
        return values or [1]

    @property
    def pixel_count(self) -> tuple[int, int]:
        """tuple[int, int]: Full (unbinned) sensor resolution (x, y) in pixels."""
        return self._pixel_count

    @property
    def pixel_pitch_um(self) -> tuple[float, float] | None:
        """tuple[float, float] | None: Centre-to-centre pixel distance (x, y) in um."""
        info = self._model_info()
        pitch = None if info is None else info.get("pixel_pitch_um")
        return None if pitch is None else (float(pitch[0]), float(pitch[1]))

    @property
    def pixel_size_um(self) -> tuple[float, float] | None:
        """tuple[float, float] | None: Photosensitive pixel width (x, y) in um; pitch if unknown."""
        info = self._model_info()
        size = None if info is None else info.get("pixel_size_um")
        return self.pixel_pitch_um if size is None else (float(size[0]), float(size[1]))

    @property
    def connection_type(self) -> str:
        """str: ``"USB3"`` or ``"GigE"``."""
        return str(self._backend.connection_type)

    @property
    def vendor(self) -> str:
        """str: Vendor name, e.g. ``"HikRobot"``."""
        return self.VENDOR

    @property
    def model(self) -> str:
        """str: Camera model name (addition to the spec)."""
        return str(self._backend.model)

    @property
    def serial(self) -> str:
        """str: Serial number of the opened camera."""
        return str(self._backend.serial)

    @property
    def index(self) -> int:
        """int: Index of the camera in the enumeration of its connection type."""
        return int(self._backend.index)

    @property
    def buffer_size(self) -> int:
        """int: Number of slots in the image buffer."""
        return self._buffer_size

    @property
    def is_recording(self) -> bool:
        """bool: True while images are acquired into the buffer (and to disk if enabled)."""
        return bool(self._backend.is_running)

    @property
    def pixel_format(self) -> str:
        """str: Pixel format name, e.g. ``"Mono8"`` (addition to the spec)."""
        return str(self._backend.pixel_layout()[3])

    @property
    def image_size(self) -> tuple[int, int]:
        """tuple[int, int]: Current image size (width, height) after ROI and binning (addition)."""
        size = self._backend.image_size() or (0, 0)
        return (int(size[0]), int(size[1]))

    @property
    def recording_file(self) -> str | None:
        """str | None: Path of the current/last ``.uc2rb`` file (addition to the spec)."""
        path = self._backend.recording_file()
        return path or None

    # --------------------------------------------------------------- settings

    def _clamp(self, name: str, value: float, rng: Range | None, unit: str) -> float:
        if rng is None:
            return value
        clamped = min(max(value, rng[0]), rng[1])
        if clamped != value:
            self._log.info(
                f"{name} {value:.6g} {unit} is outside [{rng[0]:.6g}, {rng[1]:.6g}] {unit}, "
                f"clamped to {clamped:.6g} {unit}"
            )
        return clamped

    def _report_applied(self, name: str, requested: float, applied: float | None, unit: str) -> None:
        if applied is not None and not math.isclose(requested, applied, rel_tol=1e-3, abs_tol=1e-6):
            self._log.info(f"{name}: camera applied {applied:.6g} {unit} (nearest valid value to {requested:.6g})")

    @property
    def exposure_time_ms(self) -> float | None:
        """float | None: Exposure (integration) time in ms.

        Reading returns the camera's actual value (None if not readable).
        Writing clamps to :attr:`range_exposure_time_ms`; ``None`` switches to
        automatic exposure. Not settable: warning, no change.
        """
        return self._backend.get_exposure_ms()

    @exposure_time_ms.setter
    def exposure_time_ms(self, value: float | None) -> None:
        self._require_open()
        if value is not None and not _is_real(value):
            raise TypeError(f"exposure_time_ms must be a number or None, got {type(value).__name__}")
        if value is not None and not value > 0:
            raise ValueError(f"exposure_time_ms must be > 0, got {value}")
        if not self.can_set_exposure:
            self._log.warning("exposure_time_ms cannot be set on this camera - ignored")
            return
        if value is None:
            if self._backend.set_exposure_auto():
                self._log.info("Exposure time: automatic")
            else:
                self._log.warning("Automatic exposure is not available on this camera - ignored")
            return
        v = self._clamp("exposure_time_ms", float(value), self.range_exposure_time_ms, "ms")
        if not self._backend.set_exposure_ms(v):
            raise RuntimeError("the camera refused the exposure time (see log)")
        self._report_applied("exposure_time_ms", v, self._backend.get_exposure_ms(), "ms")

    @property
    def frame_rate_per_sec(self) -> float | None:
        """float | None: Frame rate in 1/s.

        Reading returns the configured rate, or the rate the camera actually
        reaches when no limit is set. Writing clamps to
        :attr:`range_frame_rate_per_sec`; ``None`` removes the limit (free run
        at the maximum rate). Not settable: warning, no change.
        """
        return self._backend.get_frame_rate()

    @frame_rate_per_sec.setter
    def frame_rate_per_sec(self, value: float | None) -> None:
        self._require_open()
        if value is not None and not _is_real(value):
            raise TypeError(f"frame_rate_per_sec must be a number or None, got {type(value).__name__}")
        if value is not None and not value > 0:
            raise ValueError(f"frame_rate_per_sec must be > 0, got {value}")
        if not self.can_set_frame_rate:
            self._log.warning("frame_rate_per_sec cannot be set on this camera - ignored")
            return
        if value is None:
            if self._backend.disable_frame_rate_limit():
                self._log.info("Frame rate: no limit (camera runs at its maximum rate)")
            else:
                self._log.warning("The frame-rate limit cannot be switched off on this camera - ignored")
            return
        v = self._clamp("frame_rate_per_sec", float(value), self.range_frame_rate_per_sec, "1/s")
        if not self._backend.set_frame_rate(v):
            raise RuntimeError("the camera refused the frame rate (see log)")
        self._report_applied("frame_rate_per_sec", v, self._backend.get_frame_rate(), "1/s")

    @property
    def gain_db(self) -> float | None:
        """float | None: Analog gain in dB.

        Writing clamps to :attr:`range_gain_db`; ``None`` switches to automatic
        gain. Not settable: warning, no change.
        """
        return self._backend.get_gain_db()

    @gain_db.setter
    def gain_db(self, value: float | None) -> None:
        self._require_open()
        if value is not None and not _is_real(value):
            raise TypeError(f"gain_db must be a number or None, got {type(value).__name__}")
        if value is not None and value < 0:
            raise ValueError(f"gain_db must be >= 0, got {value}")
        if not self.can_set_gain:
            self._log.warning("gain_db cannot be set on this camera - ignored")
            return
        if value is None:
            if self._backend.set_gain_auto():
                self._log.info("Gain: automatic")
            else:
                self._log.warning("Automatic gain is not available on this camera - ignored")
            return
        v = self._clamp("gain_db", float(value), self.range_gain_db, "dB")
        if not self._backend.set_gain_db(v):
            raise RuntimeError("the camera refused the gain (see log)")
        self._report_applied("gain_db", v, self._backend.get_gain_db(), "dB")

    @property
    def binning(self) -> int:
        """int: Symmetric hardware binning factor (1, 2 or 4).

        Writing snaps to the nearest value of :attr:`range_binning`; ``None``
        means 1. A change while recording restarts the acquisition (new session
        file). Not settable: warning, no change.
        """
        b = self._backend.get_binning()
        return 1 if b is None else int(b)

    @binning.setter
    def binning(self, value: int | None) -> None:
        self._require_open()
        if value is None:
            value = 1
        if not _is_int(value):
            raise TypeError(f"binning must be an int or None, got {type(value).__name__}")
        if value < 1:
            raise ValueError(f"binning must be >= 1, got {value}")
        if not self.can_set_binning:
            if value != self.binning:
                self._log.warning("binning cannot be set on this camera - ignored")
            return
        allowed = self.range_binning
        target = min(allowed, key=lambda b: (abs(b - value), b))
        if target != value:
            self._log.info(f"binning {value} is not supported, using {target} (allowed: {allowed})")
        if not self._backend.set_binning(target):
            raise RuntimeError("the camera refused the binning (see log)")

    @property
    def trigger(self) -> None:
        """None: Placeholder; the camera always runs in free-run mode for now.

        Writing anything other than ``None`` logs an error and changes nothing.
        """
        return None

    @trigger.setter
    def trigger(self, value: Any) -> None:
        self._require_open()
        if value is not None:
            self._log.error(f"trigger={value!r} is not supported yet - the camera stays in free-run mode")

    @property
    def roi(self) -> Roi | None:
        """tuple[int, int, int, int] | None: (x, y, width, height) in unbinned sensor pixels.

        ``None`` means the complete sensor. Writing clamps the region to the
        sensor and snaps it to the camera's grid; a change while recording
        restarts the acquisition (new session file). Not settable: warning.
        """
        r = self._backend.get_roi()
        if r is None:
            return None
        roi = (int(r[0]), int(r[1]), int(r[2]), int(r[3]))
        if roi == (0, 0, self._pixel_count[0], self._pixel_count[1]):
            return None
        return roi

    @roi.setter
    def roi(self, value: Roi | None) -> None:
        self._require_open()
        rect: Roi | None = None
        if value is not None:
            if not isinstance(value, (tuple, list)) or len(value) != 4 or not all(_is_int(v) for v in value):
                raise TypeError("roi must be None or a tuple (x, y, width, height) of 4 ints")
            x, y, w, h = (int(v) for v in value)
            if x < 0 or y < 0 or w <= 0 or h <= 0:
                raise ValueError(f"roi needs x, y >= 0 and width, height > 0, got {tuple(value)}")
            rect = (x, y, w, h)
        if not self.can_set_roi:
            self._log.warning("roi cannot be set on this camera - ignored")
            return
        if rect is None:
            ok = self._backend.reset_roi()
        else:
            x, y, w, h = rect
            px, py = self._pixel_count
            cx, cy = min(x, px - 1), min(y, py - 1)
            cw, ch = min(w, px - cx), min(h, py - cy)
            if (cx, cy, cw, ch) != (x, y, w, h):
                self._log.info(f"roi {(x, y, w, h)} exceeds the {px}x{py} sensor, clamped to {(cx, cy, cw, ch)}")
            ok = self._backend.set_roi(cx, cy, cw, ch)
            applied = self._backend.get_roi()
            if ok and applied is not None and tuple(applied) != (cx, cy, cw, ch):
                self._log.info(f"roi: camera applied {tuple(int(v) for v in applied)} (its pixel grid)")
        if not ok:
            raise RuntimeError("the camera refused the ROI (see log)")

    # ---------------------------------------------------------------- methods

    def get_last_image(self) -> ImgData | None:
        """Return the most recently recorded image.

        Returns:
            ImgData | None: Zero-copy image, or None if no image has been
            recorded yet.
        """
        handle = self._backend.read_latest()
        return None if handle is None else ImgData._from_handle(handle)

    def get_image(self, index: int) -> ImgData:
        """Return the image held by buffer slot ``index``.

        Slots are filled cyclically (``slot = sequence % buffer_size``), so the
        slot order is not the time order - use ``ImgData.sequence`` for that.

        Args:
            index: Buffer slot, ``0 <= index < buffer_size``.

        Returns:
            ImgData: Zero-copy image of that slot.

        Raises:
            TypeError: ``index`` is not an int.
            IndexError: ``index`` is out of range.
            LookupError: The slot does not hold a completely recorded image.
        """
        if not _is_int(index):
            raise TypeError(f"index must be an int, got {type(index).__name__}")
        if not 0 <= index < self._buffer_size:
            raise IndexError(f"buffer index {index} out of range [0, {self._buffer_size})")
        handle = self._backend.read_slot(int(index))
        if handle is None:
            raise LookupError(f"buffer slot {index} holds no completely recorded image")
        return ImgData._from_handle(handle)

    def get_image_info(self) -> ImgMetaData | None:
        """Return the metadata of the current (or last) recording session.

        Returns:
            ImgMetaData | None: Session header, or None before the first start().
        """
        return ImgMetaData._from_dict(self._backend.session_header())

    def _session_comment(self) -> str:
        info = {
            "vendor": self.VENDOR,
            "model": self.model,
            "serial": self.serial,
            "connection_type": self.connection_type,
            "index": self.index,
            "pixel_format": self.pixel_format,
            "exposure_time_ms": self.exposure_time_ms,
            "gain_db": self.gain_db,
            "frame_rate_per_sec": self.frame_rate_per_sec,
            "binning": self.binning,
            "roi": self.roi,
            "pixel_count": self._pixel_count,
            "pixel_pitch_um": self.pixel_pitch_um,
            "pixel_size_um": self.pixel_size_um,
        }
        return json.dumps(info)

    def start(self) -> None:
        """Start acquiring images into the buffer (and to disk if enabled).

        Each start opens a new recording session (a new ``.uc2rb`` file). Does
        nothing if already recording.

        Raises:
            RuntimeError: The camera is closed or could not be started.
            MemoryError, OSError: See the buffer and disk checks.
        """
        self._require_open()
        if self.is_recording:
            self._log.debug("start(): already recording")
            return
        self._check_resources(log_disk_estimate=False)
        storage = str(self._image_file_path) if self._store_image_data else ""
        self._backend.set_buffer_config(self._buffer_size, storage, self._session_comment())
        if not self._backend.start():
            _logging.flush()
            message = "acquisition could not be started (see the log for the reason)"
            self._log.error(message)
            raise RuntimeError(message)

    def stop(self) -> None:
        """Stop acquiring. Recorded images stay readable until the next start()."""
        if self._closed:
            return
        self._backend.stop()

    def close(self) -> None:
        """Stop and release the camera. Images already read stay valid."""
        if self._closed:
            return
        if self._debug_window is not None:   # before the backend: the window thread reads from it
            self._debug_window.close()
            self._debug_window = None
        self._backend.close()
        self._closed = True
        _logging.flush()   # the core's shutdown messages first, then ours
        self._log.info(f"Camera closed: {self.VENDOR} {self.serial}")
        if _logging.active_logger() is self._log:
            _logging.set_active_logger(None)

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, tb: TracebackType | None
    ) -> None:
        self.close()

    def __repr__(self) -> str:
        state = "closed" if self._closed else ("recording" if self.is_recording else "idle")
        return f"<{type(self).__name__} {self.VENDOR} {self._backend.model} SN={self._backend.serial} {state}>"


def create_camera(
    vendor: str | None = None,
    connection_type: str | None = None,
    serial: str | None = None,
    index: int | None = None,
    buffer_size: int | None = None,
    start_on_init: bool = False,
    image_file_path: PathLike | None = None,
    store_image_data: bool = True,
    logger: Any = None,
    debug_window: bool = False,
    **vendor_options: Any,
) -> Camera:
    """Open a camera and return it as a :class:`Camera`.

    Args:
        vendor: ``"HikRobot"`` or ``"Simulated"`` (case-insensitive);
            None -> ``defaults.VENDOR``.
        connection_type: ``"USB3"``, ``"GigE"`` or None (any).
        serial: Serial number of the camera to open. Not together with ``index``.
        index: Index among the cameras of ``connection_type``. Without serial and
            index the first available camera is opened.
        buffer_size: Number of images in the ring buffer (>= 2);
            None -> ``defaults.BUFFER_SIZE``.
        start_on_init: Start recording right away.
        image_file_path: Directory for the ``.uc2rb`` files;
            None -> ``defaults.IMAGE_FILE_PATH``.
        store_image_data: Write every image to disk (in ``image_file_path``).
        logger: Object with ``error``, ``warning``, ``info``, ``debug``
            methods; None -> ``logging.getLogger("uc2_cam_py")``.
        debug_window: Show the newest image in a window until the camera is
            closed - the only way to open one. Without X11 an error is logged
            and the camera works without the window.
        **vendor_options: Vendor-specific extras (Simulated: ``pixel_format``,
            ``sensor_width``, ``sensor_height``, ``frame_rate``).

    Returns:
        Camera: The opened camera.

    Raises:
        TypeError, ValueError: Invalid arguments.
        IndexError: ``index`` beyond the connected cameras.
        RuntimeError: No matching camera, or it cannot be opened.
        MemoryError, OSError: Buffer or disk checks failed.
    """
    vendor = defaults.VENDOR if vendor is None else vendor
    if not isinstance(vendor, str):
        raise TypeError("vendor must be a str")
    cls = _VENDORS.get(vendor.lower())
    if cls is None:
        raise ValueError(f"unknown vendor {vendor!r}; available: {', '.join(vendors())}")

    if connection_type is not None:
        if not isinstance(connection_type, str):
            raise TypeError("connection_type must be a str or None")
        canonical = {c.lower(): c for c in CONNECTION_TYPES}.get(connection_type.lower())
        if canonical is None:
            raise ValueError(f"connection_type must be one of {CONNECTION_TYPES} or None, got {connection_type!r}")
        connection_type = canonical

    if serial is not None and (not isinstance(serial, str) or not serial):
        raise ValueError("serial must be a non-empty str or None")
    if index is not None:
        if not _is_int(index):
            raise TypeError("index must be an int or None")
        if index < 0:
            raise ValueError(f"index must be >= 0, got {index}")
    if serial is not None and index is not None:
        raise ValueError("give either serial or index, not both")

    buffer_size = defaults.BUFFER_SIZE if buffer_size is None else buffer_size
    if not _is_int(buffer_size):
        raise TypeError("buffer_size must be an int")
    if buffer_size < 2:
        raise ValueError(f"buffer_size must be >= 2, got {buffer_size}")
    for name, flag in (
        ("start_on_init", start_on_init),
        ("store_image_data", store_image_data),
        ("debug_window", debug_window),
    ):
        if not isinstance(flag, bool):
            raise TypeError(f"{name} must be a bool")
    if image_file_path is not None and not isinstance(image_file_path, (str, os.PathLike)):
        raise TypeError("image_file_path must be a str, a path or None")

    adapter = _logging.package_logger() if logger is None else _logging.LoggerAdapter(logger)
    _logging.set_active_logger(adapter)
    try:
        backend = cls._create_backend(connection_type, serial, index, **vendor_options)
    except Exception as exc:
        _logging.flush()
        adapter.error(f"Opening a {cls.VENDOR} camera failed: {exc}")
        raise
    return cls(
        backend,
        buffer_size=int(buffer_size),
        start_on_init=start_on_init,
        image_file_path=image_file_path,
        store_image_data=store_image_data,
        logger=adapter,
        debug_window=debug_window,
    )
