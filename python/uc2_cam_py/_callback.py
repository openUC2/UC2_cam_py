"""Call a function for every new image of a camera, on a thread of its own (internal).

``Camera.set_image_callback`` runs one :class:`ImageDispatcher` per camera. Its
thread waits without the GIL until the ring buffer holds a newer image, then
calls the function with the GIL. C++ threads never call into Python, so a slow
callback or a busy interpreter can never stall the camera.

The newest image wins: a callback that is slower than the camera gets the newest
image each time, never a backlog. A callback gets the images completed after it
was set: the newest image at that moment is the reference, and if there was none
yet (set before the first ``start()``), every image is new - also the first image
of the session, even if it is complete before the thread looks. While the camera
is stopped, or no callback is set, the thread sleeps on a condition without a
timeout; ``start()``, ``set_image_callback()`` and ``close()`` wake it.
"""

from __future__ import annotations

import threading
import time
import traceback
from collections.abc import Callable
from typing import Any

from ._logging import LoggerAdapter
from .data import ImgData

ImageCallback = Callable[[ImgData], Any]

#: Longest single wait for an image in ms while the camera runs. Only a safety
#: net: every event that matters (image, stop, restart, close) ends it earlier.
WAIT_SLICE_MS = 1000
#: Pause after a wait that gave up early although the camera still counts as
#: running - a state that should not last; the pause keeps it from spinning.
EARLY_RETURN_PAUSE_S = 0.01


def _name(callback: ImageCallback) -> str:
    return getattr(callback, "__qualname__", None) or repr(callback)


class ImageDispatcher:
    """Delivers the newest image of a camera to one callback, on a daemon thread.

    Args:
        wait: ``wait(newer_than, timeout_ms, any_image)`` blocks without the GIL
            until an image newer than ``newer_than`` (None: the next one, with
            ``any_image`` any one) is in the ring, and returns it; None after the
            timeout, or at once when none can come (camera stopped, closed, lost).
        latest: Returns the newest image or None (``Camera.get_last_image``).
        is_running: True while the camera acquires images.
        log: Logger of the camera.
    """

    def __init__(self, wait: Callable[[ImgData | None, int, bool], ImgData | None],
                 latest: Callable[[], ImgData | None],
                 is_running: Callable[[], bool], log: LoggerAdapter) -> None:
        self._wait = wait
        self._latest = latest
        self._is_running = is_running
        self._log = log
        # Lock order: _call_lock, then _cond. _call_lock is held while the callback
        # runs, so set() and close() wait for a running call. It is reentrant, so
        # the callback itself may call them.
        self._call_lock = threading.RLock()
        self._cond = threading.Condition()
        self._callback: ImageCallback | None = None
        self._min_interval_s = 0.0
        self._baseline: ImgData | None = None   # newest image when the callback was set
        self._generation = 0      # counts set() calls
        self._closing = False
        self._errors = 0          # exceptions of the current callback
        self._thread = threading.Thread(target=self._run, name="uc2_cam_py-image-callback", daemon=True)
        self._thread.start()

    @property
    def thread(self) -> threading.Thread:
        """The thread that calls the callback."""
        return self._thread

    def set(self, callback: ImageCallback | None, max_rate_hz: float | None) -> None:
        """Replace the callback (None: none). A running call of the old one finishes first.

        The new callback gets the images completed from now on.
        """
        baseline = self._latest()   # outside the locks: the reference for "new"
        with self._call_lock, self._cond:
            old = self._callback
            summary = self._error_summary()
            self._callback = callback
            self._baseline = baseline
            self._min_interval_s = 0.0 if max_rate_hz is None else 1.0 / float(max_rate_hz)
            self._generation += 1
            self._errors = 0
            self._cond.notify_all()
        if summary:
            self._log.error(summary)
        if old is not None and callback is not None:
            self._log.info(f"image callback {_name(old)} replaced by {_name(callback)}")
        elif callback is not None:
            rate = "" if max_rate_hz is None else f", at most {max_rate_hz:g} calls/s"
            self._log.debug(f"image callback set: {_name(callback)}{rate}")
        elif old is not None:
            self._log.debug(f"image callback removed: {_name(old)}")

    def session_changed(self) -> None:
        """Wake the thread after start(): it may be sleeping because the camera was stopped."""
        with self._cond:
            self._cond.notify_all()

    def close(self) -> None:
        """Stop calling the callback; a running call finishes first. Call :meth:`join` later."""
        with self._call_lock, self._cond:
            summary = self._error_summary()
            self._callback = None
            self._closing = True
            self._cond.notify_all()
        if summary:
            self._log.error(summary)

    def join(self, timeout: float = 2.0) -> None:
        """Wait for the thread to end (not from the thread itself, e.g. close() in the callback)."""
        if threading.current_thread() is not self._thread:
            self._thread.join(timeout)

    # ------------------------------------------------------------------ thread

    def _error_summary(self) -> str:
        """Message about the exceptions not logged one by one; call with _cond held."""
        if self._errors <= 1 or self._callback is None:
            return ""
        return (f"image callback {_name(self._callback)} raised {self._errors} exceptions in total "
                f"({self._errors - 1} not logged one by one)")

    def _idle(self) -> int | None:
        """Sleep while there is nothing to deliver. Returns the generation, None once closing."""
        with self._cond:
            while not self._closing and (self._callback is None or not self._is_running()):
                self._cond.wait()
            return None if self._closing else self._generation

    def _run(self) -> None:
        try:
            self._loop()
        except Exception as exc:  # noqa: BLE001 - logged; the camera carries on without the callback
            self._log.error(f"image callback: the thread stopped after an error: {exc}")

    def _loop(self) -> None:
        last: ImgData | None = None
        seen = -1   # generation that `last` belongs to
        while (generation := self._idle()) is not None:
            if generation != seen:   # a new callback: images from its set() on
                with self._cond:
                    last, seen = self._baseline, generation
            started = time.monotonic()
            # Without a reference (set before the first image) every image is new,
            # also one that was complete before this call.
            image = self._wait(last, WAIT_SLICE_MS, last is None)
            with self._cond:
                replaced = self._generation != seen
            if replaced:
                continue   # set() came meanwhile: measure against its reference
            if image is None:
                if time.monotonic() - started < WAIT_SLICE_MS / 2000.0:
                    # Gave up early: stopped, closed or lost. _idle() sees that at
                    # once; only if the camera still counts as running, pause.
                    with self._cond:
                        if not self._closing and self._is_running():
                            self._cond.wait(EARLY_RETURN_PAUSE_S)
                continue
            last = image
            generation = self._deliver(image)
            self._pace(started, generation)

    def _deliver(self, image: ImgData) -> int:
        """Call the current callback with ``image``; returns the generation it belonged to."""
        with self._call_lock:
            with self._cond:
                callback, generation = self._callback, self._generation
                if callback is None or self._closing:
                    return generation
            try:
                callback(image)
            except Exception:  # noqa: BLE001 - the callback must not end the thread
                with self._cond:
                    current = self._generation == generation
                    if current:
                        self._errors += 1
                    first = current and self._errors == 1
                if first:
                    self._log.error(
                        f"image callback {_name(callback)} raised an exception (further ones are only "
                        f"counted):\n{traceback.format_exc().rstrip()}"
                    )
        return generation

    def _pace(self, started: float, generation: int) -> None:
        """Keep to max_rate_hz: wait out the rest of the interval, unless set() or close() comes."""
        with self._cond:
            interval = self._min_interval_s if self._generation == generation else 0.0
            rest = started + interval - time.monotonic()
            if rest > 0:
                self._cond.wait_for(lambda: self._closing or self._generation != generation, rest)
