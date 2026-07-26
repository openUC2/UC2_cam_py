"""Type stubs for the compiled `uc2_hikpy._hik` extension module.

Hand-maintained so editors (VS Code / Pylance) show signatures and docstrings on
hover; keep the docstrings in sync with the pybind11 `.def(...)` strings in
bindings/hik_py.cpp.
"""

from __future__ import annotations

import numpy as np

class DeviceInfo:
    """Description of one enumerated camera (read-only)."""

    @property
    def transport(self) -> str:
        """str: Transport layer, e.g. 'GigE' or 'USB3'."""

    @property
    def model_name(self) -> str:
        """str: Camera model name."""

    @property
    def serial(self) -> str:
        """str: Serial number."""

    @property
    def user_name(self) -> str:
        """str: User-defined name (may be empty)."""

    @property
    def ip(self) -> str:
        """str: Current IP (GigE only, else empty)."""

    def __repr__(self) -> str: ...

class Frame:
    """A single grabbed frame.

    The `array` view is zero-copy and aliases SDK-owned memory: it is only valid
    until the next FrameGrabber.wait_for_frame() call (or stop/close). Use
    `copy()` to obtain an independent array you may keep.
    """

    @property
    def array(self) -> np.ndarray:
        """numpy.ndarray: Zero-copy view of the pixels. Mono8 -> (H, W) uint8;
        RGB8/BGR8 -> (H, W, 3) uint8; other formats -> flat (nbytes,) uint8.
        Valid only until the next wait_for_frame() call."""

    def copy(self) -> np.ndarray:
        """Return an independent, owning copy of the pixels.

        Returns:
            numpy.ndarray: A newly allocated array with the same shape and dtype
            as `array`, safe to keep beyond the next frame.
        """

    @property
    def width(self) -> int:
        """int: Image width in pixels."""

    @property
    def height(self) -> int:
        """int: Image height in pixels."""

    @property
    def pixel_format(self) -> int:
        """int: MvGvspPixelType value of the frame."""

    @property
    def frame_num(self) -> int:
        """int: Device frame/sequence number."""

    @property
    def timestamp(self) -> int:
        """int: Host-generated timestamp of the frame."""

    @property
    def lost_packets(self) -> int:
        """int: Number of packets lost while transferring this frame."""

    @property
    def nbytes(self) -> int:
        """int: Number of valid pixel bytes."""

    @property
    def itemsize(self) -> int:
        """int: Size in bytes of one channel sample (1 for 8-bit formats)."""

    @property
    def address(self) -> int:
        """int: Address of the pixel buffer, for handing to an external ring.
        The memory is valid only until the next wait_for_frame() call."""

class AutoMode:
    """Auto-regulation mode for exposure or gain."""

    Off: AutoMode
    """Firmware regulation disabled; the manual value is used."""
    Once: AutoMode
    """Regulate once, then hold."""
    Continuous: AutoMode
    """Regulate continuously."""

class Roi:
    """Region of interest (in pixels) on the sensor."""

    @property
    def width(self) -> int:
        """int: ROI width in pixels."""

    @property
    def height(self) -> int:
        """int: ROI height in pixels."""

    @property
    def offset_x(self) -> int:
        """int: Horizontal offset from the left sensor edge."""

    @property
    def offset_y(self) -> int:
        """int: Vertical offset from the top sensor edge."""

    def __repr__(self) -> str: ...

class FrameGrabber:
    """Continuous, callback-driven grabber exposing the latest frame zero-copy.

    Acquisition runs on an SDK worker thread using the OneByOne strategy. There
    is no ring buffer here; only the most recent frame is exposed. Typical
    lifecycle: open() -> start() -> wait_for_frame()* -> stop()/close().
    """

    def __init__(self) -> None:
        """Construct an unopened grabber. The MVS SDK must already be initialised."""

    def open(self, index: int) -> bool:
        """Enumerate devices and open one.

        Args:
            index (int): Zero-based device index into the enumeration.

        Returns:
            bool: True if the device was opened (or already open).
        """

    def set_node_num(self, n: int) -> None:
        """Set the SDK internal buffer-node count used from the next start().

        Args:
            n (int): Number of nodes (clamped to at least 3).

        Returns:
            None
        """

    def start(self) -> bool:
        """Configure node count and grab strategy, register the image callback
        and begin grabbing. Call after open().

        Returns:
            bool: True on success.
        """

    def wait_for_frame(self, timeout_ms: int = 1000) -> Frame | None:
        """Block until the next frame is ready, a timeout elapses, or the grabber stops.

        Args:
            timeout_ms (int): Maximum time to wait, in milliseconds (default 1000).

        Returns:
            Frame | None: The latest frame, or None on timeout/stop. The frame's
            zero-copy view is valid only until the next wait_for_frame() call.
        """

    def stop(self) -> None:
        """Stop grabbing, unregister the callback and release held buffers.

        Returns:
            None
        """

    def close(self) -> None:
        """Stop grabbing and close the device.

        Returns:
            None
        """

    @property
    def is_running(self) -> bool:
        """bool: True while grabbing is active."""

    @property
    def frames_received(self) -> int:
        """int: Total frames delivered by the SDK callback."""

    @property
    def frames_dropped(self) -> int:
        """int: Frames superseded before the consumer read them."""

    @property
    def payload_size(self) -> int:
        """int: Camera PayloadSize in bytes (0 until open())."""

    @property
    def width(self) -> int:
        """int: Width of the most recent frame (0 if none yet)."""

    @property
    def height(self) -> int:
        """int: Height of the most recent frame (0 if none yet)."""

    @property
    def pixel_format(self) -> int:
        """int: MvGvspPixelType of the most recent frame (0 if none)."""

    def set_exposure_time(self, microseconds: float) -> bool:
        """Set the exposure time (switches auto-exposure Off so the value sticks).

        Args:
            microseconds (float): Exposure time in microseconds.

        Returns:
            bool: True on success.
        """

    def get_exposure_time(self) -> float:
        """Read the current exposure time.

        Returns:
            float: Exposure time in microseconds.

        Raises:
            RuntimeError: If the value cannot be read.
        """

    def get_exposure_time_range(self) -> tuple[float, float]:
        """Read the exposure-time limits.

        Returns:
            tuple[float, float]: (min, max) exposure time in microseconds.
        """

    def set_gain(self, db: float) -> bool:
        """Set the analog gain (switches auto-gain Off so the value sticks).

        Args:
            db (float): Gain in decibels.

        Returns:
            bool: True on success.
        """

    def get_gain(self) -> float:
        """Read the current gain.

        Returns:
            float: Gain in decibels.

        Raises:
            RuntimeError: If the value cannot be read.
        """

    def get_gain_range(self) -> tuple[float, float]:
        """Read the gain limits.

        Returns:
            tuple[float, float]: (min, max) gain in decibels.
        """

    def set_frame_rate(self, fps: float) -> bool:
        """Enable manual frame-rate control and set the acquisition frame rate.

        Args:
            fps (float): Requested frame rate in frames per second.

        Returns:
            bool: True on success.
        """

    def get_frame_rate(self) -> float:
        """Read the configured acquisition frame rate.

        Returns:
            float: Frame rate in frames per second.

        Raises:
            RuntimeError: If the value cannot be read.
        """

    def get_resulting_frame_rate(self) -> float:
        """Read the resulting (actual) frame rate the camera can achieve.

        Returns:
            float: Resulting frame rate in frames per second.

        Raises:
            RuntimeError: If the node is unavailable.
        """

    def get_frame_rate_range(self) -> tuple[float, float]:
        """Read the acquisition frame-rate limits.

        Returns:
            tuple[float, float]: (min, max) frame rate in frames per second.
        """

    def set_exposure_auto(self, mode: AutoMode) -> bool:
        """Set the auto-exposure mode.

        Args:
            mode (AutoMode): Off, Once or Continuous.

        Returns:
            bool: True on success.
        """

    def get_exposure_auto(self) -> AutoMode:
        """Read the auto-exposure mode.

        Returns:
            AutoMode: The current mode.
        """

    def set_gain_auto(self, mode: AutoMode) -> bool:
        """Set the auto-gain mode.

        Args:
            mode (AutoMode): Off, Once or Continuous.

        Returns:
            bool: True on success.
        """

    def get_gain_auto(self) -> AutoMode:
        """Read the auto-gain mode.

        Returns:
            AutoMode: The current mode.
        """

    def set_roi(
        self, width: int, height: int, offset_x: int = 0, offset_y: int = 0
    ) -> bool:
        """Set the region of interest (values are clamped/aligned to the camera grid).

        If grabbing is active this briefly stops and restarts the stream, because
        the SDK only accepts ROI changes while stopped.

        Args:
            width (int): ROI width in pixels.
            height (int): ROI height in pixels.
            offset_x (int): Horizontal offset from the left edge (default 0).
            offset_y (int): Vertical offset from the top edge (default 0).

        Returns:
            bool: True on success.
        """

    def get_roi(self) -> Roi:
        """Read the current region of interest.

        Returns:
            Roi: The current width/height/offset_x/offset_y.
        """

    def get_sensor_size(self) -> tuple[int, int]:
        """Read the full sensor size (WidthMax, HeightMax).

        Returns:
            tuple[int, int]: (width_max, height_max) in pixels.
        """

def initialize() -> bool:
    """Initialise the MVS SDK. Call once before list_devices()/run()/FrameGrabber.

    Returns:
        bool: True on success.
    """

def finalize() -> None:
    """Finalise the MVS SDK and release its resources.

    Returns:
        None
    """

def list_devices() -> list[DeviceInfo]:
    """Enumerate the connected GigE/USB3 cameras.

    Returns:
        list[DeviceInfo]: One entry per detected camera (empty if none).
    """

def run(
    index: int = -1,
    frames: int = 0,
    snapshot: str = "",
    display: bool = True,
) -> int:
    """Run a one-shot interactive capture session.

    Args:
        index (int): Device index, or -1 to prompt on the terminal (default -1).
        frames (int): Stop after N frames, or 0 to run until Enter/window close (default 0).
        snapshot (str): Path for the first-frame snapshot (.bmp/.jpg/.png/.tif); empty to skip.
        display (bool): Open a preview window when X11 support is available (default True).

    Returns:
        int: 0 on success, 2 if no camera was found, 1 on other errors.
    """
