"""Pythonic management wrapper around the C++ continuous frame grabber.

The heavy lifting (acquisition thread, zero-copy buffers) lives in the compiled
:class:`uc2_hikpy._hik.FrameGrabber`; this module only adds convenience:
lifecycle management, a context manager and a frame iterator.

Example:
    from uc2_hikpy import Grabber

    with Grabber(index=0, node_num=5) as g:
        for frame in g.frames(timeout_ms=1000):
            process(frame.array)      # zero-copy view, valid until the next frame
            if done:
                break
"""

from __future__ import annotations

from typing import Iterator, Optional, Sequence, Tuple

from ._hik import AutoMode, Frame, FrameGrabber, Roi


class Grabber:
    """High-level manager for a single continuously grabbing camera.

    Args:
        index (int): Zero-based device index to open. Defaults to 0.
        node_num (int): SDK internal buffer-node count (clamped to >= 3 in C++).
            Defaults to 5.
    """

    def __init__(self, index: int = 0, node_num: int = 5) -> None:
        self._grabber = FrameGrabber()
        self._index = index
        self._node_num = node_num

    def open(self) -> bool:
        """Enumerate devices and open the configured one.

        Returns:
            bool: True if the device was opened (or was already open).
        """
        return self._grabber.open(self._index)

    def start(self) -> bool:
        """Open the device if needed, then begin continuous grabbing.

        Returns:
            bool: True if grabbing started successfully.
        """
        if not self.open():
            return False
        self._grabber.set_node_num(self._node_num)
        return self._grabber.start()

    def wait_for_frame(self, timeout_ms: int = 1000) -> Optional[Frame]:
        """Block until the next frame is ready, a timeout elapses, or stop().

        Args:
            timeout_ms (int): Maximum time to wait, in milliseconds. Defaults to 1000.

        Returns:
            Frame | None: The latest frame, or None on timeout/stop. The frame's
            ``array`` view is valid only until the next call.
        """
        return self._grabber.wait_for_frame(timeout_ms)

    def frames(self, timeout_ms: int = 1000) -> Iterator[Frame]:
        """Yield frames continuously until the grabber is stopped.

        Timeouts (no frame within ``timeout_ms``) are skipped silently; the
        iterator ends once grabbing is no longer running.

        Args:
            timeout_ms (int): Per-frame wait timeout, in milliseconds. Defaults to 1000.

        Yields:
            Frame: Each freshly grabbed frame.
        """
        while self._grabber.is_running:
            frame = self._grabber.wait_for_frame(timeout_ms)
            if frame is not None:
                yield frame

    def stop(self) -> None:
        """Stop grabbing and release held buffers.

        Returns:
            None
        """
        self._grabber.stop()

    def close(self) -> None:
        """Stop grabbing and close the device.

        Returns:
            None
        """
        self._grabber.close()

    @property
    def is_running(self) -> bool:
        """bool: True while grabbing is active."""
        return self._grabber.is_running

    @property
    def frames_received(self) -> int:
        """int: Total frames delivered by the SDK callback."""
        return self._grabber.frames_received

    @property
    def frames_dropped(self) -> int:
        """int: Frames superseded before the consumer read them."""
        return self._grabber.frames_dropped

    @property
    def payload_size(self) -> int:
        """int: Camera PayloadSize in bytes (0 until the device is open)."""
        return self._grabber.payload_size

    @property
    def width(self) -> int:
        """int: Width of the most recent frame (0 if none yet)."""
        return self._grabber.width

    @property
    def height(self) -> int:
        """int: Height of the most recent frame (0 if none yet)."""
        return self._grabber.height

    @property
    def pixel_format(self) -> int:
        """int: MvGvspPixelType of the most recent frame (0 if none yet)."""
        return self._grabber.pixel_format

    # --- Camera parameters (require the device to be open) -------------------

    def set_exposure_time(self, microseconds: float) -> bool:
        """Set the exposure time (auto-exposure is switched Off).

        Args:
            microseconds (float): Exposure time in microseconds.

        Returns:
            bool: True on success.
        """
        return self._grabber.set_exposure_time(microseconds)

    def get_exposure_time(self) -> float:
        """Read the current exposure time.

        Returns:
            float: Exposure time in microseconds.
        """
        return self._grabber.get_exposure_time()

    def get_exposure_time_range(self) -> Tuple[float, float]:
        """Read the exposure-time limits.

        Returns:
            tuple[float, float]: (min, max) exposure time in microseconds.
        """
        return self._grabber.get_exposure_time_range()

    def set_gain(self, db: float) -> bool:
        """Set the analog gain (auto-gain is switched Off).

        Args:
            db (float): Gain in decibels.

        Returns:
            bool: True on success.
        """
        return self._grabber.set_gain(db)

    def get_gain(self) -> float:
        """Read the current gain.

        Returns:
            float: Gain in decibels.
        """
        return self._grabber.get_gain()

    def get_gain_range(self) -> Tuple[float, float]:
        """Read the gain limits.

        Returns:
            tuple[float, float]: (min, max) gain in decibels.
        """
        return self._grabber.get_gain_range()

    def set_frame_rate(self, fps: float) -> bool:
        """Enable manual frame-rate control and set the acquisition frame rate.

        Args:
            fps (float): Requested frame rate in frames per second.

        Returns:
            bool: True on success.
        """
        return self._grabber.set_frame_rate(fps)

    def get_frame_rate(self) -> float:
        """Read the configured acquisition frame rate.

        Returns:
            float: Frame rate in frames per second.
        """
        return self._grabber.get_frame_rate()

    def get_resulting_frame_rate(self) -> float:
        """Read the resulting (actual) frame rate the camera can achieve.

        Returns:
            float: Resulting frame rate in frames per second.
        """
        return self._grabber.get_resulting_frame_rate()

    def get_frame_rate_range(self) -> Tuple[float, float]:
        """Read the acquisition frame-rate limits.

        Returns:
            tuple[float, float]: (min, max) frame rate in frames per second.
        """
        return self._grabber.get_frame_rate_range()

    def set_exposure_auto(self, mode: AutoMode) -> bool:
        """Set the auto-exposure mode.

        Args:
            mode (AutoMode): Off, Once or Continuous.

        Returns:
            bool: True on success.
        """
        return self._grabber.set_exposure_auto(mode)

    def get_exposure_auto(self) -> AutoMode:
        """Read the auto-exposure mode.

        Returns:
            AutoMode: The current mode.
        """
        return self._grabber.get_exposure_auto()

    def set_gain_auto(self, mode: AutoMode) -> bool:
        """Set the auto-gain mode.

        Args:
            mode (AutoMode): Off, Once or Continuous.

        Returns:
            bool: True on success.
        """
        return self._grabber.set_gain_auto(mode)

    def get_gain_auto(self) -> AutoMode:
        """Read the auto-gain mode.

        Returns:
            AutoMode: The current mode.
        """
        return self._grabber.get_gain_auto()

    def set_roi(
        self, width: int, height: int, offset_x: int = 0, offset_y: int = 0
    ) -> bool:
        """Set the region of interest (clamped/aligned to the camera grid).

        If grabbing is active the stream is briefly stopped and restarted, since
        the SDK only accepts ROI changes while stopped. The zero-copy policy is
        preserved: subsequent frames simply carry the new (smaller) shape.

        Args:
            width (int): ROI width in pixels.
            height (int): ROI height in pixels.
            offset_x (int): Horizontal offset from the left edge (default 0).
            offset_y (int): Vertical offset from the top edge (default 0).

        Returns:
            bool: True on success.
        """
        return self._grabber.set_roi(width, height, offset_x, offset_y)

    def get_roi(self) -> Roi:
        """Read the current region of interest.

        Returns:
            Roi: The current width/height/offset_x/offset_y.
        """
        return self._grabber.get_roi()

    def get_sensor_size(self) -> Tuple[int, int]:
        """Read the full sensor size.

        Returns:
            tuple[int, int]: (width_max, height_max) in pixels.
        """
        return self._grabber.get_sensor_size()

    def configure(
        self,
        *,
        exposure_us: Optional[float] = None,
        gain_db: Optional[float] = None,
        fps: Optional[float] = None,
        roi: Optional[Sequence[int]] = None,
        exposure_auto: Optional[AutoMode] = None,
        gain_auto: Optional[AutoMode] = None,
    ) -> None:
        """Apply several camera parameters in one call.

        Only the arguments you pass are applied. ROI is applied last (it may
        restart the stream). Manual exposure/gain override the corresponding
        auto mode, so pass an ``*_auto`` argument only if you want auto control.

        Args:
            exposure_us (float | None): Exposure time in microseconds.
            gain_db (float | None): Gain in decibels.
            fps (float | None): Acquisition frame rate in frames per second.
            roi (Sequence[int] | None): (width, height) or (width, height,
                offset_x, offset_y) in pixels.
            exposure_auto (AutoMode | None): Auto-exposure mode.
            gain_auto (AutoMode | None): Auto-gain mode.

        Returns:
            None

        Raises:
            ValueError: If ``roi`` does not have 2 or 4 elements.
        """
        if exposure_auto is not None:
            self.set_exposure_auto(exposure_auto)
        if gain_auto is not None:
            self.set_gain_auto(gain_auto)
        if exposure_us is not None:
            self.set_exposure_time(exposure_us)
        if gain_db is not None:
            self.set_gain(gain_db)
        if fps is not None:
            self.set_frame_rate(fps)
        if roi is not None:
            if len(roi) not in (2, 4):
                raise ValueError("roi must be (width, height) or (width, height, offset_x, offset_y)")
            width, height = roi[0], roi[1]
            offset_x, offset_y = (roi[2], roi[3]) if len(roi) == 4 else (0, 0)
            self.set_roi(width, height, offset_x, offset_y)

    def __enter__(self) -> "Grabber":
        """Open the device and start grabbing on entry.

        Returns:
            Grabber: This instance.

        Raises:
            RuntimeError: If the camera could not be opened or started.
        """
        if not self.start():
            raise RuntimeError(f"failed to open/start camera at index {self._index}")
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        """Stop grabbing and close the device on exit.

        Returns:
            None
        """
        self.close()
