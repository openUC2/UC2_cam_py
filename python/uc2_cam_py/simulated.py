"""Simulated camera for tests and development without hardware.

It produces a Gaussian spot that walks row by row over a 5 x 4 grid of the
sensor, through the same ring-buffer path as a real camera. The expected spot
centre of every frame is known (``camera.spot_position(sequence)``), so tests
can check the image content, not just its shape.
"""

from __future__ import annotations

from typing import Any

from . import _hik
from .camera import Camera, register_vendor

MODELS: dict[str, dict[str, Any]] = {
    # Pitch of a typical 1/1.2" global-shutter sensor, so the non-None path of
    # pixel_pitch_um is exercised; no real camera is implied.
    "UC2-SIM": {"pixel_pitch_um": (5.86, 5.86)},
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
        cls, connection_type: str | None, serial: str | None, index: int | None, **options: Any
    ) -> Any:
        if index not in (None, 0):
            raise IndexError(f"index {index} out of range (1 simulated camera)")
        allowed = {"pixel_format", "sensor_width", "sensor_height", "frame_rate"}
        unknown = set(options) - allowed
        if unknown:
            raise TypeError(f"unexpected options for {cls.VENDOR}: {', '.join(sorted(unknown))}")
        return _hik.SimCamera(
            serial=serial or "SIM-0001",
            connection_type=connection_type or "USB3",
            **options,
        )

    def _model_info(self) -> dict[str, Any] | None:
        return MODELS.get(self.model)

    def spot_position(self, sequence: int) -> tuple[float, float]:
        """Expected spot centre (x, y) in sensor pixels of the frame ``sequence``."""
        x, y = self._backend.spot_position(sequence)
        return (float(x), float(y))
