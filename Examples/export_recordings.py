"""Export recordings (.uc2rb) as OME-TIFF and read them back - how to use uc2_cam_py.exporter.

    uv run python Examples/export_recordings.py [FOLDER]
        Exports every recording in FOLDER (default: the recording folder, ~/UC2_recordings).

    uv run python Examples/export_recordings.py --demo [FOLDER]
        Records one second with the simulated camera first (into FOLDER, or a temporary
        folder), so the example also runs without a camera and without recordings.

Each recording becomes <name>.ome.tif next to it. The script then shows what is in
the OME-TIFF: the images, the session header of the recording and the frame header
of every image.
"""

from __future__ import annotations

import argparse
import json
import tempfile
import time
from pathlib import Path

import uc2_cam_py
from uc2_cam_py import exporter


def record_demo(folder: Path) -> None:
    """One second with the simulated camera, recorded to disk."""
    with uc2_cam_py.create_camera(
        vendor="Simulated", store_image_data=True, image_file_path=folder,
        sensor_width=640, sensor_height=480, frame_rate=20.0,
    ) as cam:
        cam.start()
        time.sleep(1.0)
        cam.stop()


def show(path: str) -> None:
    """Export one recording and print what the OME-TIFF holds."""
    # export_as_ome_tiff() writes the file and returns it as an *open* tifffile.TiffFile:
    # use it in a with-block so it is closed again.
    with exporter.export_as_ome_tiff(path) as tif:
        series = tif.series[0]                        # all images: (T, H, W) or (T, H, W, 3)
        print(f"{path}\n  -> {tif.filehandle.path}")
        print(f"  images: {series.shape}, {series.dtype}, axes {series.axes}")

        # The session header of the recording: tag SESSION_HEADER_TAG on the first page
        # (it is also in the OME-XML, tif.ome_metadata, as a MapAnnotation).
        page0 = tif.pages.get(0, aspage=True)
        session = json.loads(page0.tags[exporter.SESSION_HEADER_TAG].value)
        print(f"  camera {session['camera_id']}, recorded with uc2_cam_py {session['software_version']}")

        # The frame header of every image: tag FRAME_HEADER_TAG on the image's own page.
        # aspage=True: tifffile otherwise hands out a lightweight page without its tags.
        headers = [
            json.loads(tif.pages.get(i, aspage=True).tags[exporter.FRAME_HEADER_TAG].value)
            for i in range(len(tif.pages))
        ]
        times = [((h["time_before_ns"] + h["time_after_ns"]) // 2 - session["start_mono_ns"]) / 1e9 for h in headers]
        for h, t in list(zip(headers, times, strict=True))[:3]:
            print(f"  sequence {h['sequence']:>4}  at {t:8.3f} s  (slot {h['slot']})")
        if len(times) > 1:
            print(f"  ... {len(headers)} images, {(len(times) - 1) / (times[-1] - times[0]):.1f} images/s")

        # One image as a numpy array.
        first = series.asarray(key=0)
        print(f"  first image: min {first.min()}, max {first.max()}, mean {first.mean():.1f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder", nargs="?", help="folder with .uc2rb recordings (default: the recording folder)")
    parser.add_argument("--demo", action="store_true", help="record with the simulated camera first")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="uc2_demo_") as tmp:
        folder = args.folder
        if args.demo:
            folder = folder or tmp
            record_demo(Path(folder))

        recordings = exporter.list_raw_files(folder)   # None: the default recording folder
        if not recordings:
            print(f"No recordings in {folder or uc2_cam_py.defaults.IMAGE_FILE_PATH} - try --demo.")
            return 1
        for path in recordings:
            show(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
