"""HikRobot (MVS SDK) cameras."""

from __future__ import annotations

from typing import Any

from . import _hik
from .camera import Camera, register_vendor

#: Static data per camera model, for what the firmware does not report.
#: Keys: "pixel_pitch_um" (x, y), "pixel_size_um" (x, y), "pixel_count" (x, y).
#: Intentionally empty: add a model only with values from its datasheet, e.g.
#:     MODELS["MV-CA023-10UM"] = {"pixel_pitch_um": (5.86, 5.86)}
MODELS: dict[str, dict[str, Any]] = {}


@register_vendor
class HikRobotCamera(Camera):
    """Camera of the vendor HikRobot, driven through the MVS SDK.

    Firmware units are converted to the interface units (ExposureTime is in
    microseconds on the camera, milliseconds here).
    """

    VENDOR = "HikRobot"

    @classmethod
    def _create_backend(
        cls, connection_type: str | None, serial: str | None, index: int | None, **options: Any
    ) -> Any:
        if options:
            raise TypeError(f"unexpected options for {cls.VENDOR}: {', '.join(sorted(options))}")
        return _hik.HikCamera(connection_type or "", serial or "", -1 if index is None else index)

    def _model_info(self) -> dict[str, Any] | None:
        return MODELS.get(self.model)
