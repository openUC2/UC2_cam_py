"""Simulated camera for tests and development without hardware.

It produces a Gaussian spot that walks row by row over a 5 x 4 grid of the
sensor, through the same ring-buffer path as a real camera. The spot sits on a
constant offset of 10 % of its height and carries shot noise (variance = mean,
Poisson statistics). The expected spot
centre of every frame is known (``camera.spot_position(sequence)``), so tests
can check the image content, not just its shape.
"""

from __future__ import annotations

from typing import Any

from . import _hik, _process
from .camera import Camera, CameraInfo, register_vendor

#: Model name and default serial of the simulated camera (as set by the C++ SimCamera).
MODEL = "UC2-SIM"
SERIAL = "SIM-0001"

#: Vendor options and the types they accept.
_OPTION_TYPES: dict[str, type | tuple[type, ...]] = {
    "pixel_format": str,
    "sensor_width": int,
    "sensor_height": int,
    "frame_rate": (int, float),
}
_OPTION_NAMES = {"pixel_format": "a str", "sensor_width": "an int", "sensor_height": "an int", "frame_rate": "a number"}

MODELS: dict[str, dict[str, Any]] = {
    # Pitch of a typical 1/1.2" global-shutter sensor, so the non-None path of
    # pixel_pitch_um is exercised; no real camera is implied.
    MODEL: {"pixel_pitch_um": (5.86, 5.86)},
}


@register_vendor
class SimulatedCamera(Camera):
    """Camera of the vendor "Simulated".

    Vendor options of :func:`uc2_cam_py.create_camera`:
        pixel_format: ``"Mono16"`` (default) or ``"Mono8"``.
        sensor_width, sensor_height: Sensor size (default 1920 x 1200).
        frame_rate: Initial frame rate in 1/s (default 10).

    Exposure and gain are accepted and reported, but do not change the image.
    """

    VENDOR = "Simulated"

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
        if index not in (None, 0):
            raise IndexError(f"index {index} out of range (1 simulated camera)")
        unknown = set(options) - set(_OPTION_TYPES)
        if unknown:
            raise TypeError(f"unexpected options for {cls.VENDOR}: {', '.join(sorted(unknown))}")
        for name, value in options.items():
            # Checked here for both paths: the camera process receives the options as text.
            if isinstance(value, bool) or not isinstance(value, _OPTION_TYPES[name]):
                raise TypeError(f"{name} must be {_OPTION_NAMES[name]}, got {type(value).__name__}")
        serial = serial or SERIAL
        connection_type = connection_type or "USB3"
        if separate_process:
            return _process.open_camera(cls.VENDOR, connection_type, serial, None, options)
        return _hik.SimCamera(serial=serial, connection_type=connection_type, **options)

    @classmethod
    def _discover(cls) -> list[CameraInfo]:
        # Exactly one simulated camera; it has no real connection.
        return [CameraInfo(vendor=cls.VENDOR, model=MODEL, serial=SERIAL, connection_type="")]

    def _model_info(self) -> dict[str, Any] | None:
        return MODELS.get(self.model)

    def spot_position(self, sequence: int) -> tuple[float, float]:
        """Expected spot centre (x, y) in sensor pixels of the frame ``sequence``."""
        x, y = self._backend.spot_position(sequence)
        return (float(x), float(y))
