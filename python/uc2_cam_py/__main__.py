"""Command-line entry point: ``python -m uc2_cam_py`` or the ``uc2_cam_py`` script.

Lists every camera that can be opened (all vendors, see
:func:`uc2_cam_py.list_cameras`), opens the one whose index is typed in and
shows each new image in the debug window as soon as it is in the ring buffer.
Nothing is written to disk. Stops on Enter, Ctrl+C or when the window is closed.
"""

from __future__ import annotations

import argparse
import logging
import threading

import uc2_cam_py


def _print_table(cameras: list[uc2_cam_py.CameraInfo]) -> None:
    rows = [("Index", "Name", "Vendor", "Connection", "Serial")]
    rows += [(str(i), c.model, c.vendor, c.connection_type or "-", c.serial) for i, c in enumerate(cameras)]
    widths = [max(len(row[col]) for row in rows) for col in range(len(rows[0]))]
    for row in rows:
        print("  ".join(cell.ljust(width) for cell, width in zip(row, widths, strict=True)).rstrip())


def _choose(cameras: list[uc2_cam_py.CameraInfo]) -> uc2_cam_py.CameraInfo | None:
    """Ask for an index until it is valid; None on an empty answer, EOF or Ctrl+C."""
    last = len(cameras) - 1
    while True:
        try:
            answer = input(f"Camera index (0-{last}, Enter = quit): ").strip()
        except (EOFError, KeyboardInterrupt):
            return None
        if not answer:
            return None
        if answer.isdigit() and int(answer) <= last:
            return cameras[int(answer)]
        print(f"Please enter a number from 0 to {last}.")


def _enter_pressed() -> threading.Event:
    """Event that is set once Enter is pressed (or stdin is closed)."""
    pressed = threading.Event()

    def read() -> None:
        try:
            input()
        except EOFError:
            pass
        pressed.set()

    threading.Thread(target=read, name="uc2_cam_py-stdin", daemon=True).start()
    return pressed


def main() -> int:
    argparse.ArgumentParser(
        prog="uc2_cam_py",
        description="List the available cameras, open one and show its images live in a debug window "
                    "(nothing is written to disk).",
    ).parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S")

    cameras = uc2_cam_py.list_cameras()
    if not cameras:
        print("No cameras found.")
        return 1
    _print_table(cameras)
    info = _choose(cameras)
    if info is None:
        return 0

    try:
        cam = uc2_cam_py.create_camera(
            vendor=info.vendor,
            serial=info.serial,
            store_image_data=False,
            debug_window=True,
            start_on_init=True,
        )
    except (RuntimeError, OSError, MemoryError, IndexError, ValueError):
        return 1   # create_camera() has logged the reason

    with cam:
        print("Live view running - press Enter or Ctrl+C to stop, or close the window.")
        pressed = _enter_pressed()
        window = cam._debug_window
        try:
            while not pressed.wait(0.2):
                if window is not None and window.opened and not window.is_open:
                    break   # closed by the user
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
