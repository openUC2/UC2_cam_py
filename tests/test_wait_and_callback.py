"""New images without polling: wait_for_image() and set_image_callback() (interface spec §7.10).

"At once" means well below the timeout of the call: the wait must be ended by
the event (stop, close, lost camera process), not run into its timeout.
"""

from __future__ import annotations

import itertools
import math
import os
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from pathlib import Path

import numpy as np
import pytest

import uc2_cam_py
from uc2_cam_py import ImgData

from conftest import wait_for_frames

ROOT = Path(__file__).resolve().parent.parent
#: Upper bound for "at once" in seconds; generous for sanitizer builds and the Pi.
AT_ONCE_S = 1.5


@pytest.fixture(params=[pytest.param(True, id="Simulated"), pytest.param(False, id="Simulated-in-process")])
def sim(request, open_camera) -> uc2_cam_py.Camera:
    """The simulated camera in its own process and in this one (400 x 200, 100 fps)."""
    return open_camera("Simulated", separate_process=request.param)


def eventually(cond: Callable[[], bool], timeout: float = 5.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.005)
    return cond()


def in_thread(delay_s: float, action: Callable[[], object]) -> threading.Thread:
    """Run ``action`` after ``delay_s`` on another thread (started already)."""

    def run() -> None:
        time.sleep(delay_s)
        action()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def start_slow(cam: uc2_cam_py.Camera, fps: float = 0.2) -> ImgData:
    """Start at ``fps`` (0.2: the next image comes after 5 s) and return the first image."""
    cam.frame_rate_per_sec = fps
    cam.start()
    wait_for_frames(cam, 1)
    img = cam.get_last_image()
    assert img is not None
    return img


# ------------------------------------------------------------ wait_for_image


def test_wait_returns_the_next_and_then_a_newer_image(camera) -> None:
    camera.start()
    wait_for_frames(camera, 1)
    before = camera.get_last_image().sequence
    first = camera.wait_for_image(timeout_s=5.0)
    assert isinstance(first, ImgData)
    assert first.sequence > before          # the next image, not the one already there
    newer = camera.wait_for_image(timeout_s=5.0, newer_than=first)
    assert newer is not None and newer.sequence > first.sequence
    assert newer.data.shape == first.data.shape
    assert not newer.data.flags.writeable   # zero-copy view, as from get_last_image()


def test_wait_returns_the_newest_image_not_the_one_after(sim) -> None:
    sim.start()
    wait_for_frames(sim, 1)
    old = sim.get_last_image()
    wait_for_frames(sim, old.sequence + 10)
    img = sim.wait_for_image(timeout_s=5.0, newer_than=old)   # at once: newer ones are there
    assert img.sequence >= old.sequence + 9


def test_wait_runs_into_the_timeout_when_no_image_comes(sim) -> None:
    first = start_slow(sim)
    started = time.monotonic()
    assert sim.wait_for_image(timeout_s=0.3, newer_than=first) is None
    assert 0.25 <= time.monotonic() - started < AT_ONCE_S


def test_wait_returns_none_at_once_when_no_image_can_come(camera) -> None:
    started = time.monotonic()
    assert camera.wait_for_image(timeout_s=10.0) is None   # never started
    camera.start()
    wait_for_frames(camera, 2)
    first = camera.get_last_image()
    camera.stop()
    last = camera.get_last_image()
    assert camera.wait_for_image(timeout_s=10.0, newer_than=last) is None
    assert camera.wait_for_image(timeout_s=10.0) is None
    assert time.monotonic() - started < AT_ONCE_S
    if last.sequence > first.sequence:   # a newer image that is there is still returned
        assert camera.wait_for_image(timeout_s=0, newer_than=first).sequence == last.sequence


def test_stopping_during_the_wait_ends_it_at_once(sim) -> None:
    start_slow(sim)
    stopper = in_thread(0.2, sim.stop)
    started = time.monotonic()
    assert sim.wait_for_image(timeout_s=10.0) is None
    assert time.monotonic() - started < AT_ONCE_S
    stopper.join()
    assert not sim.is_recording


def test_closing_during_the_wait_ends_it_at_once(sim) -> None:
    start_slow(sim)
    closer = in_thread(0.2, sim.close)
    started = time.monotonic()
    assert sim.wait_for_image(timeout_s=10.0) is None
    assert time.monotonic() - started < AT_ONCE_S
    closer.join()


def test_a_restart_during_the_wait_is_followed_into_the_new_session(sim) -> None:
    first = start_slow(sim)

    def change() -> None:
        sim.roi = (0, 0, 200, 100)

    changer = in_thread(0.2, change)
    started = time.monotonic()
    img = sim.wait_for_image(timeout_s=10.0, newer_than=first)
    assert time.monotonic() - started < AT_ONCE_S + 1.0
    changer.join()
    assert img is not None
    assert img.sequence == 0                    # the first image of the new session ...
    assert img.data.shape == (100, 200, 1)      # ... with its new shape


def test_an_image_of_an_older_session_is_older_than_every_current_one(sim) -> None:
    sim.start()
    wait_for_frames(sim, 30)
    old = sim.get_last_image()
    sim.frame_rate_per_sec = 0.5     # the new session would need a minute to pass old.sequence
    sim.roi = (0, 0, 200, 100)       # restart: a new session
    started = time.monotonic()
    img = sim.wait_for_image(timeout_s=10.0, newer_than=old)
    assert time.monotonic() - started < AT_ONCE_S
    assert img is not None and img.sequence < old.sequence
    assert img.data.shape == (100, 200, 1)


def test_a_killed_camera_process_ends_the_wait_at_once(open_camera) -> None:
    cam = open_camera(separate_process=True)
    start_slow(cam)
    pid = cam.process_id
    killer = in_thread(0.2, lambda: os.kill(pid, signal.SIGKILL))
    started = time.monotonic()
    assert cam.wait_for_image(timeout_s=10.0) is None
    assert time.monotonic() - started < AT_ONCE_S
    killer.join()
    with pytest.raises(RuntimeError):
        cam.wait_for_image(timeout_s=1.0)


def test_waiting_on_a_closed_camera_raises(sim) -> None:
    sim.close()
    with pytest.raises(RuntimeError):
        sim.wait_for_image(timeout_s=1.0)


def test_the_wait_releases_the_gil(sim) -> None:
    first = start_slow(sim)
    waiter = threading.Thread(target=sim.wait_for_image, args=(1.0, first), daemon=True)
    waiter.start()
    time.sleep(0.05)
    spins, end = 0, time.monotonic() + 0.3
    while time.monotonic() < end:   # would not run before the wait ends if it held the GIL
        spins += 1
    assert waiter.is_alive()
    assert spins > 1000
    waiter.join()


@pytest.mark.parametrize("timeout", ["1", None, True, [1.0]])
def test_wait_rejects_a_timeout_that_is_no_number(sim, timeout) -> None:
    with pytest.raises(TypeError):
        sim.wait_for_image(timeout)


@pytest.mark.parametrize("timeout", [-0.001, math.inf, math.nan, 5e6])
def test_wait_rejects_a_timeout_out_of_range(sim, timeout) -> None:
    with pytest.raises(ValueError):
        sim.wait_for_image(timeout)


def test_wait_rejects_newer_than_that_was_not_read_from_a_camera(sim) -> None:
    with pytest.raises(TypeError):
        sim.wait_for_image(0.1, newer_than=np.zeros((2, 2)))
    made = ImgData(data=np.zeros((2, 2, 1), np.uint16), time_before_ns=0, time_after_ns=0,
                   time_ns=0, sequence=0, slot=0)
    with pytest.raises(ValueError):
        sim.wait_for_image(0.1, newer_than=made)


# ------------------------------------------------------------ set_image_callback


class Recorder:
    """A callback that records what it gets, optionally slowly."""

    def __init__(self, delay_s: float = 0.0) -> None:
        self.delay_s = delay_s
        self.sequences: list[int] = []
        self.shapes: list[tuple[int, ...]] = []
        self.threads: set[str] = set()
        self._lock = threading.Lock()

    def __call__(self, img: ImgData) -> None:
        with self._lock:
            self.sequences.append(img.sequence)
            self.shapes.append(img.data.shape)
            self.threads.add(threading.current_thread().name)
        if self.delay_s:
            time.sleep(self.delay_s)

    @property
    def count(self) -> int:
        with self._lock:
            return len(self.sequences)


def test_the_callback_gets_increasing_images_on_a_thread_of_its_own(camera) -> None:
    rec = Recorder()
    camera.set_image_callback(rec)
    camera.start()
    assert eventually(lambda: rec.count >= 5)
    camera.set_image_callback(None)
    seq = rec.sequences
    assert all(b > a for a, b in itertools.pairwise(seq))
    assert rec.threads == {"uc2_cam_py-image-callback"}


def test_a_callback_set_before_start_gets_the_first_image(sim) -> None:
    rec = Recorder()
    sim.set_image_callback(rec)
    sim.frame_rate_per_sec = 0.2   # image 0 at once, image 1 only after 5 s
    started = time.monotonic()
    sim.start()
    assert eventually(lambda: rec.count >= 1, timeout=AT_ONCE_S)
    assert time.monotonic() - started < AT_ONCE_S
    assert rec.sequences[0] == 0


def test_a_callback_set_while_running_gets_only_later_images(sim) -> None:
    sim.start()
    wait_for_frames(sim, 5)
    before = sim.get_last_image().sequence
    rec = Recorder()
    sim.set_image_callback(rec)
    assert eventually(lambda: rec.count >= 1)
    assert rec.sequences[0] > before


def test_a_slow_callback_gets_the_newest_image_without_a_backlog(sim) -> None:
    rec = Recorder(delay_s=0.1)   # 100 fps: about 10 images per call
    sim.start()
    sim.set_image_callback(rec)
    assert eventually(lambda: rec.count >= 4)
    sim.set_image_callback(None)
    gaps = np.diff(rec.sequences)
    assert (gaps >= 3).all()                                        # skipped, not queued
    assert sim.get_last_image().sequence - rec.sequences[-1] < 30   # the last one was recent


def test_max_rate_hz_limits_the_calls(sim) -> None:
    rec = Recorder()
    sim.start()
    sim.set_image_callback(rec, max_rate_hz=10)
    time.sleep(1.0)
    sim.set_image_callback(None)
    assert 5 <= rec.count <= 12   # 100 fps camera, at most 10 calls per second


def test_after_removal_no_call_starts_and_a_running_one_was_waited_for(sim) -> None:
    inside = threading.Event()
    calls = []

    def slow(img: ImgData) -> None:
        inside.set()
        time.sleep(0.2)
        calls.append(img.sequence)
        inside.clear()

    sim.start()
    sim.set_image_callback(slow)
    assert inside.wait(5.0)
    sim.set_image_callback(None)   # while the call is running
    assert not inside.is_set()     # it had finished
    count = len(calls)
    time.sleep(0.3)
    assert len(calls) == count


def test_replacing_the_callback_logs_info_and_switches(sim, rec_logger) -> None:
    first, second = Recorder(), Recorder()
    sim.start()
    sim.set_image_callback(first)
    assert eventually(lambda: first.count >= 1)
    sim.set_image_callback(second)
    count = first.count
    assert eventually(lambda: second.count >= 2)
    assert first.count == count
    assert rec_logger.has("INFO", "replaced")


def test_an_exception_is_logged_once_and_the_calls_go_on(sim, rec_logger) -> None:
    calls = []

    def failing(img: ImgData) -> None:
        calls.append(img.sequence)
        raise ValueError("broken callback")

    sim.start()
    sim.set_image_callback(failing)
    assert eventually(lambda: len(calls) >= 5)
    sim.set_image_callback(None)
    errors = rec_logger.messages("ERROR")
    single = [m for m in errors if "raised an exception" in m]
    assert len(single) == 1
    assert "Traceback" in single[0] and "broken callback" in single[0]
    assert any("exceptions in total" in m for m in errors)


def test_the_callback_may_replace_and_remove_itself(sim) -> None:
    second_called = threading.Event()

    def second(img: ImgData) -> None:
        sim.set_image_callback(None)   # from inside: must not deadlock
        second_called.set()

    def first(img: ImgData) -> None:
        sim.set_image_callback(second)

    sim.start()
    sim.set_image_callback(first)
    assert second_called.wait(5.0)
    assert eventually(lambda: sim._image_callback._callback is None)


def test_the_callback_may_close_the_camera(sim) -> None:
    closed = threading.Event()

    def closing(img: ImgData) -> None:
        sim.close()   # from inside: must not deadlock
        closed.set()

    sim.start()
    sim.set_image_callback(closing)
    assert closed.wait(5.0)
    thread = sim._image_callback.thread
    thread.join(2.0)
    assert not thread.is_alive()


def test_the_callback_survives_stop_start_and_a_restart(sim) -> None:
    rec = Recorder()
    sim.set_image_callback(rec)    # before the start: the thread sleeps until then
    sim.start()
    assert eventually(lambda: rec.count >= 2)
    sim.stop()
    count = rec.count
    time.sleep(0.2)
    assert rec.count == count      # stopped: no calls
    sim.start()
    assert eventually(lambda: rec.count >= count + 2)
    sim.roi = (0, 0, 200, 100)     # restart
    assert eventually(lambda: rec.shapes[-1] == (100, 200, 1))


def test_the_callback_thread_sleeps_while_nothing_comes(sim) -> None:
    rec = Recorder()
    sim.set_image_callback(rec)
    dispatcher = sim._image_callback
    waits = []
    original = dispatcher._wait

    def counting(*args):
        waits.append(args)
        return original(*args)

    dispatcher._wait = counting
    time.sleep(0.5)
    assert waits == []             # never started: no wait at all
    start_slow(sim)                # one image, the next after 5 s
    assert eventually(lambda: rec.count >= 1)
    time.sleep(1.5)
    assert len(waits) <= 4         # running without images: about one wake-up per second
    sim.stop()
    time.sleep(0.2)
    stopped = len(waits)
    time.sleep(0.5)
    assert len(waits) == stopped   # stopped: no wake-ups


def test_close_ends_the_callback_thread(sim) -> None:
    sim.start()
    sim.set_image_callback(Recorder(delay_s=0.05))
    thread = sim._image_callback.thread
    started = time.monotonic()
    sim.close()
    assert time.monotonic() - started < 2.0
    assert not thread.is_alive()


def test_set_image_callback_checks_its_arguments(sim) -> None:
    with pytest.raises(TypeError):
        sim.set_image_callback(42)
    with pytest.raises(TypeError):
        sim.set_image_callback(Recorder(), max_rate_hz="fast")
    for bad in (0, -1.0, math.inf, math.nan):
        with pytest.raises(ValueError):
            sim.set_image_callback(Recorder(), max_rate_hz=bad)
    sim.close()
    with pytest.raises(RuntimeError):
        sim.set_image_callback(Recorder())
    sim.set_image_callback(None)   # removing is fine on a closed camera


# ------------------------------------------------------------ the example


def test_the_example_shows_all_three_ways() -> None:
    """Examples/newest_image.py: polling, waiting, callback with the simulated camera."""
    result = subprocess.run(
        [sys.executable, str(ROOT / "Examples" / "newest_image.py")],
        check=False, capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stderr
    out = result.stdout
    assert out.count("1 polling:") == 1
    assert out.count("2 waiting:") == 3
    assert out.count("3 callback:") == 3
    assert "returned None" in out
