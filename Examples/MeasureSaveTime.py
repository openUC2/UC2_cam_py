"""How long does writing one image to disk take - and reading it out of the camera?

    uv run python Analysis/MeasureSaveTime.py            # the defaults below
    uv run python Analysis/MeasureSaveTime.py --help     # all options

Opens the first connected HikRobot camera, records NUM_IMAGES images to disk with
the package's own disk writer (``start(write_stats=True)``) and prints mean, std,
min and max of

- write per image: the writer's ``writev()`` of frame header plus pixels,
- readout per image: ``time_after_ns - time_before_ns``, the copy from the SDK
  buffer into the ring slot,
- frame interval: the time between two images reaching the host,

plus the final ``fsync()`` and the throughput. The recording ends up in
OUTPUT_FILE, its metadata (with the write statistics) next to it as ``.json``.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import uc2_cam_py
from uc2_cam_py import _uc2rb

# ------------------------------------------------------------------ settings
# The defaults; each can be overridden on the command line (see --help).

DEBUG_WINDOW = False                                        # live view while recording
BUFFER_SIZE = 20                                            # slots in the ring buffer
OUTPUT_FILE = Path.cwd() / "Analysis" / "SampleData.uc2rb"  # the recording (+ .json next to it)
NUM_IMAGES = 100                                            # images to record
FRAME_RATE = None                                           # images/s; None: leave the camera's setting

#: Longest wait for the next image before the run is given up, in seconds.
WAIT_TIMEOUT_S = 5.0
MIB = 1024 * 1024


@dataclass
class Recording:
    """What one run produced: the file and what the camera was set to."""

    path: Path
    camera: str
    settings: str


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Measure the disk write time and the readout time per image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--images", type=int, default=NUM_IMAGES, help="images to record")
    p.add_argument("--buffer-size", type=int, default=BUFFER_SIZE, help="slots in the ring buffer")
    p.add_argument("--output", type=Path, default=OUTPUT_FILE, help="the recording file")
    p.add_argument("--debug-window", action=argparse.BooleanOptionalAction, default=DEBUG_WINDOW,
                   help="show the live view while recording")
    p.add_argument("--frame-rate", type=float, default=FRAME_RATE,
                   help="images per second (default: leave the camera's setting)")
    p.add_argument("--simulated", action="store_true",
                   help="use the simulated camera instead of a HikRobot camera")
    args = p.parse_args()
    if args.images < 2:
        p.error("--images must be at least 2 (the frame interval needs two images)")
    if args.buffer_size < 2:
        p.error("--buffer-size must be at least 2")
    return args


def find_camera(simulated: bool) -> uc2_cam_py.CameraInfo | None:
    """The first camera of the wanted vendor, or None."""
    vendor = "Simulated" if simulated else "HikRobot"
    return next((c for c in uc2_cam_py.list_cameras() if c.vendor == vendor), None)


def record(info: uc2_cam_py.CameraInfo, args: argparse.Namespace, folder: Path) -> Recording:
    """Record args.images images into ``folder`` with timed disk writes; returns the file."""
    with uc2_cam_py.create_camera(
        vendor=info.vendor, serial=info.serial, buffer_size=args.buffer_size,
        image_file_path=folder, store_image_data=True, debug_window=args.debug_window,
    ) as cam:
        if args.frame_rate is not None:
            cam.frame_rate_per_sec = args.frame_rate
        width, height = cam.image_size
        exposure = cam.exposure_time_ms
        rate = cam.frame_rate_per_sec
        camera = f"{cam.vendor} {cam.model}, SN {cam.serial}" + (f" ({cam.connection_type})" if cam.connection_type else "")
        settings = (
            f"{width} x {height} {cam.pixel_format}, "
            f"exposure {'auto' if exposure is None else f'{exposure:.2f} ms'}, "
            f"frame rate {'free-run' if rate is None else f'{rate:.1f}/s'}\n"
            f"           buffer {args.buffer_size} slots, debug window {'on' if args.debug_window else 'off'}"
        )

        cam.start(write_stats=True)
        path = cam.recording_file
        if path is None:
            raise RuntimeError("the camera does not record to disk (see the log)")
        last = None
        while last is None or last.sequence < args.images - 1:
            new = cam.wait_for_image(timeout_s=WAIT_TIMEOUT_S, newer_than=last)
            if new is None:
                raise RuntimeError(f"no new image within {WAIT_TIMEOUT_S:g} s - is the camera delivering?")
            last = new
        cam.stop()   # closes the file: fsync, then the sidecar gets the write statistics
    return Recording(Path(path), camera, settings)


def move(src: Path, dst: Path) -> bool:
    """Move the recording and its sidecar to ``dst``; returns True if it replaced one."""
    replaced = dst.exists()
    os.replace(src, dst)   # same file system: the work folder is next to dst
    sidecar = json.loads(src.with_suffix(".json").read_text())
    sidecar["container"] = dst.name
    dst.with_suffix(".json").write_text(json.dumps(sidecar, indent=2) + "\n")
    src.with_suffix(".json").unlink()
    return replaced


def row(name: str, values_ms: np.ndarray, note: str = "") -> str:
    v = np.asarray(values_ms, dtype=np.float64)
    return f"{name:<20}{v.mean():>10.3f}{v.std():>10.3f}{v.min():>10.3f}{v.max():>10.3f}   {note}"


def report(rec: Recording, path: Path) -> None:
    """Print the statistics of the recording at ``path``."""
    sidecar = json.loads(path.with_suffix(".json").read_text())
    ws = sidecar.get("writeStats")
    if ws is None:
        raise RuntimeError(f"{path.with_suffix('.json')} has no write statistics")
    with open(path, "rb") as f:
        meta = _uc2rb.read_session_header(f, path)
        frames = _uc2rb.frame_records(f, path, meta)
    readout = np.array([r.time_after_ns - r.time_before_ns for r in frames]) / 1e6
    interval = np.diff([r.time_before_ns for r in frames]) / 1e6

    saved, dropped = ws["framesSaved"], ws["framesDropped"]
    record_bytes = meta.frame_bytes + _uc2rb.FRAME_HEADER.itemsize
    write_s = (ws["frameWriteMeanNs"] * saved + ws["closeNs"]) / 1e9
    to_ms = 1e-6

    print(f"Camera     {rec.camera}")
    print(f"Image      {rec.settings}")
    print(f"File       {path} (+ .json)")
    print(f"           {len(frames)} images, {dropped} dropped, {path.stat().st_size / MIB:.1f} MiB, "
          f"{meta.frame_bytes / MIB:.2f} MiB per image")
    print()
    print(f"{'':<20}{'mean':>10}{'std':>10}{'min':>10}{'max':>10}   [ms]")
    print(f"{'write per image':<20}{ws['frameWriteMeanNs'] * to_ms:>10.3f}{ws['frameWriteStdNs'] * to_ms:>10.3f}"
          f"{ws['frameWriteMinNs'] * to_ms:>10.3f}{ws['frameWriteMaxNs'] * to_ms:>10.3f}   (1)")
    print(row("readout per image", readout, "(2)"))
    print(row("frame interval", interval, f"(3) = {1000.0 / interval.mean():.1f} images/s"))
    print(f"{'close (fsync)':<20}{ws['closeNs'] * to_ms:>10.3f}")
    print()
    print(f"disk throughput     {saved * record_bytes / MIB / write_s:>10.1f} MiB/s   (bytes / (writes + close))")
    print(f"camera data rate    {meta.frame_bytes / MIB / (interval.mean() / 1000.0):>10.1f} MiB/s   "
          "(image bytes / mean frame interval)")
    print()
    print("(1) writev() of frame header + pixels by the package's disk writer. It lands in the page")
    print("    cache; the kernel holds it up when the disk falls behind. The fsync() at the end")
    print("    writes what is still pending.")
    print("(2) Copy from the SDK buffer into the ring slot (time_after_ns - time_before_ns).")
    print("    Exposure and USB transfer happen before it and are not measured.")
    print("(3) Time between two images reaching the host.")
    if saved != len(frames):
        print(f"Note: the writer timed {saved} images, the file holds {len(frames)}.")


def main() -> int:
    args = parse_args()
    info = find_camera(args.simulated)
    if info is None:
        what = "simulated" if args.simulated else "HikRobot"
        print(f"No {what} camera found (uc2_cam_py.list_cameras()). Is it connected? "
              "Without a camera: --simulated", file=sys.stderr)
        return 1

    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Record next to the target, on the same disk: that is the disk being measured.
    work = Path(tempfile.mkdtemp(prefix=".measure_", dir=output.parent))
    try:
        print(f"Recording {args.images} images from {info.vendor} {info.model} (SN {info.serial}) ...\n")
        rec = record(info, args, work)
        if move(rec.path, output):
            print(f"(replaced the previous {output.name})\n")
        report(rec, output)
    except RuntimeError as exc:
        print(f"Measurement failed: {exc}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
