"""Forward the C++ core's log messages to a Python logger.

The C++ side never touches the GIL: its log sink appends to a queue. The daemon
thread started here drains it with ``_hik._pop_log`` (which waits *without*
the GIL) and calls the active logger *with* the GIL - so a busy Python thread
can never stall the camera thread, and no lock-order deadlock between the GIL
and C++ mutexes is possible.

C++ messages carry no camera reference. They go to the logger of the most
recently created camera, or to the package logger ``uc2_cam_py``.
"""

from __future__ import annotations

import atexit
import itertools
import logging
import threading
from typing import Any, Optional

from . import _hik, defaults

# Printable on purpose: the message passes through a C string, a NUL would cut it.
_FLUSH_PREFIX = "[uc2_cam_py.flush]"


class LoggerAdapter:
    """Uniform ``error/warning/info/debug`` front for any logger-like object.

    Accepts a :class:`logging.Logger` or any object providing ``error``,
    ``warning`` (or ``warn``), ``info`` and ``debug`` callables taking one
    message string.
    """

    def __init__(self, target: Any) -> None:
        warning = getattr(target, "warning", None) or getattr(target, "warn", None)
        missing = [
            name
            for name, fn in (
                ("error", getattr(target, "error", None)),
                ("warning", warning),
                ("info", getattr(target, "info", None)),
                ("debug", getattr(target, "debug", None)),
            )
            if not callable(fn)
        ]
        if missing:
            raise TypeError(
                "logger must provide error/warning/info/debug methods; missing: " + ", ".join(missing)
            )
        self.target = target
        self.error = target.error
        self.warning = warning
        self.info = target.info
        self.debug = target.debug

    def log(self, level: int, message: str) -> None:
        """Dispatch by ``logging`` level number."""
        if level >= logging.ERROR:
            self.error(message)
        elif level >= logging.WARNING:
            self.warning(message)
        elif level >= logging.INFO:
            self.info(message)
        else:
            self.debug(message)


def package_logger() -> LoggerAdapter:
    """The adapter for ``logging.getLogger(defaults.LOGGER_NAME)``."""
    return LoggerAdapter(logging.getLogger(defaults.LOGGER_NAME))


_lock = threading.Lock()
_active: Optional[LoggerAdapter] = None
_thread: Optional[threading.Thread] = None
_stop = threading.Event()
_flush_ids = itertools.count()
_flush_seen: "set[int]" = set()
_flush_cv = threading.Condition()


def set_active_logger(adapter: Optional[LoggerAdapter]) -> None:
    """Route C++ messages to ``adapter`` (None: back to the package logger)."""
    global _active
    with _lock:
        _active = adapter


def active_logger() -> LoggerAdapter:
    with _lock:
        return _active if _active is not None else package_logger()


def _dispatch(level: int, message: str) -> None:
    if message.startswith(_FLUSH_PREFIX):
        with _flush_cv:
            _flush_seen.add(int(message[len(_FLUSH_PREFIX):]))
            _flush_cv.notify_all()
        return
    try:
        active_logger().log(level, message)
    except Exception:  # a broken user logger must not kill the forwarder
        logging.getLogger(defaults.LOGGER_NAME).exception("logger raised while forwarding a C++ message")


def _run() -> None:
    while not _stop.is_set():
        for level, message in _hik._pop_log(200, 256):
            _dispatch(level, message)
        dropped = _hik._log_dropped()
        if dropped:
            active_logger().warning(f"{dropped} C++ log message(s) dropped (queue full)")


def start() -> None:
    """Install the queue sink and start the forwarding thread (idempotent)."""
    global _thread
    if _thread is not None and _thread.is_alive():
        return
    _stop.clear()
    _hik._set_log_forwarding(True)
    _thread = threading.Thread(target=_run, name="uc2_cam_py-log", daemon=True)
    _thread.start()


def stop() -> None:
    """Stop forwarding, deliver what is still queued and restore stderr output."""
    global _thread
    if _thread is None:
        return
    _stop.set()
    _hik._wake_log()
    _thread.join(timeout=2.0)
    _thread = None
    _hik._set_log_forwarding(False)
    for level, message in _hik._pop_log(0, 1_000_000):
        _dispatch(level, message)


def flush(timeout: float = 2.0) -> bool:
    """Wait until every C++ message logged before this call has been forwarded.

    Returns:
        bool: False if the forwarder did not catch up within ``timeout``.
    """
    if _thread is None:
        return True
    marker = next(_flush_ids)
    _hik._emit_log(logging.ERROR, f"{_FLUSH_PREFIX}{marker}")
    with _flush_cv:
        ok = _flush_cv.wait_for(lambda: marker in _flush_seen, timeout)
        _flush_seen.discard(marker)
    return ok


atexit.register(stop)
