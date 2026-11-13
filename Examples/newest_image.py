"""Three ways to get the newest image: polling, waiting, callback.

    uv run python Examples/newest_image.py

Runs with the simulated camera, so no hardware is needed.

1. Polling - get_last_image() returns at once. Good when your program is busy
   anyway and only now and then wants the current image.
2. Waiting - wait_for_image() blocks (without the GIL) until a newer image is
   there. Good for a loop that processes image after image.
3. Callback - set_image_callback() calls your function for every new image, on
   a thread of its own. Good when your program is mostly idle.

All three return zero-copy images: check img.is_valid() after processing, or
take img.data.copy() to keep one.
"""

from __future__ import annotations

import threading
import time

import uc2_cam_py
from uc2_cam_py import ImgData


def describe(img: ImgData) -> str:
    return f"image {img.sequence:3d}  mean {img.data.mean():7.1f}  valid {img.is_valid()}"


def main() -> None:
    with uc2_cam_py.create_camera(vendor="Simulated", sensor_width=640, sensor_height=480,
                                  frame_rate=20.0) as cam:
        cam.start()
        time.sleep(0.2)

        # 1. Polling: ask whenever it suits you. None only before the first image.
        img = cam.get_last_image()
        print("1 polling:", describe(img) if img else "no image yet")

        # 2. Waiting: the next three images, each newer than the one before.
        for _ in range(3):
            img = cam.wait_for_image(timeout_s=1.0, newer_than=img)
            if img is None:   # timeout, or the camera was stopped meanwhile
                break
            print("2 waiting:", describe(img))

        # 3. Callback: called on its own thread. Hand work over to your main
        #    thread from here (here: an Event; in Qt a signal).
        seen: list[int] = []
        enough = threading.Event()

        def on_image(new: ImgData) -> None:
            seen.append(new.sequence)
            print("3 callback:", describe(new), f"(thread {threading.current_thread().name})")
            if len(seen) == 3:
                enough.set()

        cam.set_image_callback(on_image)
        enough.wait(timeout=2.0)
        cam.set_image_callback(None)   # when this returns, no further call starts

        # Once the camera is stopped, no new image can come: the wait returns at once.
        cam.stop()
        started = time.monotonic()
        img = cam.wait_for_image(timeout_s=5.0, newer_than=cam.get_last_image())
        print(f"stopped:   wait_for_image() returned {img} after {time.monotonic() - started:.3f} s")


if __name__ == "__main__":
    main()
