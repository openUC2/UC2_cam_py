"""HikRobot (MVS SDK) cameras."""

from __future__ import annotations

from typing import Any

from . import _hik, _process
from .camera import Camera, CameraInfo, register_vendor

#: Static data per camera model, for what the firmware does not report.
#: Keys: "pixel_pitch_um" (x, y), "pixel_size_um" (x, y), "pixel_count" (x, y).
#: Intentionally empty: add a model only with values from its datasheet, e.g.
#:     MODELS["MV-CA023-10UM"] = {"pixel_pitch_um": (5.86, 5.86)}
MODELS: dict[str, dict[str, Any]] = {}

#: Logged on import and raised by create_camera in a build without the MVS SDK.
NO_MVS_MESSAGE = (
    "HikRobot cameras are not available: uc2_cam_py was built without the MVS SDK, "
    "so only the 'Simulated' camera can be used. To add HikRobot support, put the MVS "
    "packages into External/hik, run `uv run fetch_mvs` and reinstall uc2_cam_py "
    "(see External/hik/README.md)."
)


@register_vendor
class HikRobotCamera(Camera):
    """Camera of the vendor HikRobot, driven through the MVS SDK.

    Firmware units are converted to the interface units (ExposureTime is in
    microseconds on the camera, milliseconds here).
    """

    VENDOR = "HikRobot"

    @classmethod
    def _create_backend(
        cls,
        connection_type: str | None,
        serial: str | None,
        index: int | None,
        *,
        separate_process: bool,
        **options: Any,
    ) -> Any:
        reason = cls._unavailable()
        if reason is not None:
            raise RuntimeError(reason)
        if options:
            raise TypeError(f"unexpected options for {cls.VENDOR}: {', '.join(sorted(options))}")
        if separate_process:
            return _process.open_camera(cls.VENDOR, connection_type, serial, index, {})
        return _hik.HikCamera(connection_type or "", serial or "", -1 if index is None else index)

    @classmethod
    def _discover(cls) -> list[CameraInfo]:
        if cls._unavailable() is not None:
            return []
        return [
            CameraInfo(vendor=cls.VENDOR, model=d.model_name, serial=d.serial, connection_type=d.transport)
            for d in _hik.list_devices()
        ]

    @classmethod
    def _unavailable(cls) -> str | None:
        return None if _hik.HAVE_MVS else NO_MVS_MESSAGE

    def _model_info(self) -> dict[str, Any] | None:
        return MODELS.get(self.model)
