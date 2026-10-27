"""Forward the C++ core's log messages to a Python logger.

The C++ side never touches the GIL: its log sink appends to a queue. The daemon
thread started here drains it with ``_hik._pop_log`` (which waits *without*
the GIL) and calls the active logger *with* the GIL - so a busy Python thread
can never stall the camera thread, and no lock-order deadlock between the GIL
and C++ mutexes is possible.

Messages of a camera process (``separate_process=True``) carry the source tag
of their camera and go to that camera's logger. Messages of the C++ core in this
process carry no camera reference: they go to the logger of the most recently
created camera, or to the package logger ``uc2_cam_py``.
"""

from __future__ import annotations

import atexit
import itertools
import logging
import threading
from collections.abc import Callable
from typing import Any

from . import _hik, defaults

# Printable on purpose: the message passes through a C string, a NUL would cut it.
_FLUSH_PREFIX = "[uc2_cam_py.flush]"

LogFn = Callable[[str], Any]


class LoggerAdapter:
    """Uniform ``error/warning/info/debug`` front for any logger-like object.

    Accepts a :class:`logging.Logger` or any object providing ``error``,
    ``warning`` (or ``warn``), ``info`` and ``debug`` callables taking one
    message string.
    """

    def __init__(self, target: Any) -> None:
        def find(*names: str) -> LogFn | None:
            for name in names:
                fn = getattr(target, name, None)
                if callable(fn):
                    return fn
            return None

        error, warning, info, debug = find("error"), find("warning", "warn"), find("info"), find("debug")
        # One explicit None check (not a comprehension) so type checkers see all four as callable.
        if error is None or warning is None or info is None or debug is None:
            found = {"error": error, "warning": warning, "info": info, "debug": debug}
            missing = [name for name, fn in found.items() if fn is None]
            raise TypeError(
                "logger must provide error/warning/info/debug methods; missing: " + ", ".join(missing)
            )
        self.target = target
        self.error: LogFn = error
        self.warning: LogFn = warning
        self.info: LogFn = info
        self.debug: LogFn = debug

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
_active: LoggerAdapter | None = None
_sources: dict[int, LoggerAdapter] = {}
_thread: threading.Thread | None = None
_stop = threading.Event()
_flush_ids = itertools.count()
_flush_seen: set[int] = set()
_flush_cv = threading.Condition()


def set_active_logger(adapter: LoggerAdapter | None) -> None:
    """Route C++ messages to ``adapter`` (None: back to the package logger)."""
    global _active
    with _lock:
        _active = adapter


def active_logger() -> LoggerAdapter:
    with _lock:
        return _active if _active is not None else package_logger()


def register_source(source: int, adapter: LoggerAdapter) -> None:
    """Route C++ messages tagged ``source`` (one camera process) to ``adapter``."""
    with _lock:
        _sources[source] = adapter


def unregister_source(source: int) -> None:
    """Forget ``source``; its late messages go to the active logger again."""
    with _lock:
        _sources.pop(source, None)


def _target(source: int) -> LoggerAdapter:
    with _lock:
        adapter = _sources.get(source) if source else None
        if adapter is None:
            adapter = _active
    return adapter if adapter is not None else package_logger()


def _dispatch(level: int, message: str, source: int = 0) -> None:
    if message.startswith(_FLUSH_PREFIX):
        with _flush_cv:
            _flush_seen.add(int(message[len(_FLUSH_PREFIX):]))
            _flush_cv.notify_all()
        return
    try:
        _target(source).log(level, message)
    except Exception:  # a broken user logger must not kill the forwarder
        logging.getLogger(defaults.LOGGER_NAME).exception("logger raised while forwarding a C++ message")


def _run() -> None:
    while not _stop.is_set():
        for level, message, source in _hik._pop_log(200, 256):
            _dispatch(level, message, source)
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
    for level, message, source in _hik._pop_log(0, 1_000_000):
        _dispatch(level, message, source)


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
