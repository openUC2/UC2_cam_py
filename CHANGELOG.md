# Changelog

All notable changes to `uc2_cam_py` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/) (see "Versioning" in the README).

## [Unreleased]

## [2.3.0] - 2026-11-14

### Added

- **Write statistics on request.** `Camera.start(write_stats=True)` has the disk writer time
  every image it writes (the one `writev()` of frame header plus pixels) and the final
  `fsync()`. When a recording file is closed, its `.json` sidecar gets a `"writeStats"` block:
  `framesSaved`, `framesDropped`, `frameWriteMeanNs`, `frameWriteStdNs`, `frameWriteMinNs`,
  `frameWriteMaxNs`, `closeNs`. Without the switch nothing is timed and the sidecar is as
  before. C++: `RingBuffer::Config::writeStats`, `CameraBase::BufferConfig::writeStats`,
  `FileStreamSink::writeStats()`.
- `Analysis/MeasureSaveTime.py`: records from the first connected HikRobot camera and prints
  mean, std, min and max of the write time and the readout time per image, the frame interval,
  the final `fsync()` and the throughput. Settings (debug window, buffer size, target file,
  number of images) at the top of the script or on the command line.

### Changed

- The sidecar is written via a temporary file and `rename()`, so an interrupted write never
  leaves half a sidecar.
- IPC protocol 3: `SetBufferConfig` carries the write-statistics flag. Camera process and
  package always come from the same build (checked at start), so nothing changes for users.

### Fixed

- `Docs/RingBuff_REF.md`: the visible line numbers of the source links had not been updated
  with their targets in 2.2.0.

## [2.2.0] - 2026-11-13

### Added

- **New images without polling.** Besides `get_last_image()`, which stays as it is, a program
  can now wait or be called:
  - `Camera.wait_for_image(timeout_s, newer_than=None)` waits without the GIL for an image
    newer than `newer_than` (`None`: the next one) and returns the newest. It returns `None`
    after the timeout, or at once when no new image can come: the camera is stopped (also
    during the wait), closed, or its camera process is lost. A restart for `roi`/`binning`
    during the wait is followed into the new session.
  - `Camera.set_image_callback(callback, max_rate_hz=None)` calls `callback(img)` for every new
    image, on a thread of its own (never on a camera thread). The newest image wins, so a slow
    callback never builds up a backlog; `max_rate_hz` limits the calls. While the camera is
    stopped the thread sleeps. One callback per camera.
  - `Examples/newest_image.py` shows all three ways with the simulated camera.
  - Semantics in the interface spec, new §7.10.
- C++: `RingBuffer::waitForFrame()` takes a `WaitScope`; with `WaitScope::Session` it gives up
  at once while recording is off. `RingBuffer::abortWaits()` ends every wait on a ring whose
  producer process died.

### Fixed

- **Waiting for a frame slept through a stop or a restart.** `stop()`, `start()` and every
  ROI/binning change end a session; a reader waiting in the old one was not woken and slept
  until its timeout. `stopRecording()` now wakes the waiters, in every process. Before the
  first `start()` the wait also slept for the full timeout; it now returns at once.
- **A frame of an older session was taken for a newer one, or the other way round.** The
  sequence starts at 0 in every session, so a reader that waited for "a frame after
  sequence n" of the old session got none until the new session passed n. Newer is now decided
  by the session (the ring) first, then by the sequence. The debug window used to work around
  this with a short timeout.

### Changed

- C++: `CameraBase::waitForFrame()` and `RemoteCamera::waitForFrame()` are replaced by
  `waitForImage(out, timeoutMs, after)`, which pins the newest image and follows restarts. The
  private Python binding `wait_for_frame` became `wait_for_image`.

## [2.1.2] - 2026-11-13

### Fixed

- **Ring buffer in `FullPolicy::Wait`: the producer could stall for up to `waitTimeoutMs`
  when a consumer registered, or streaming to disk started, while it was writing.** The
  consumer entry counted as active before its start cursor was set, so the producer could
  see a stale cursor and go to sleep with nobody left to wake it. It then overwrote the
  frame after the timeout with a "still holds unacknowledged frame" warning. The disk writer
  now also acknowledges up to where it starts reading, so a ring that fills up while
  streaming starts cannot hold the producer back either. The cameras of `uc2_cam_py` use
  `FullPolicy::Overwrite` and were not affected; only C++ users of the ring buffer with
  `FullPolicy::Wait` were.

## [2.1.1] - 2026-10-30

### Fixed

- **Ring buffer on CPUs that reorder memory accesses (aarch64, e.g. Raspberry Pi 5).** The
  seqlock that tells a reader whether a frame was overwritten while it read it lacked two
  fences. Without them an ARM CPU may let the reader's pixel reads slip past the final check,
  or make a new frame's pixels visible before the slot is marked as being written. A
  half-overwritten frame could then pass `ImgData.is_valid()` (and the disk writer's check).
  x86 orders these accesses anyway, so nothing changes there: the fences emit no instruction.

### Added

- `Docs/ARM_Checkliste.md`: step-by-step checklist for the first build and test run on a
  Raspberry Pi 5 / aarch64, with a results table. The package has not run on ARM hardware yet.

## [2.1.0] - 2026-10-29

### Added

- **OME-TIFF export of recordings: `uc2_cam_py.exporter`.**
  - `list_raw_files(folder=None)` lists the `.uc2rb` recordings of a folder (default: the
    recording folder) as absolute paths.
  - `export_as_ome_tiff(path, *, output_path=None)` writes `<name>.ome.tif` and returns it as
    an open `tifffile.TiffFile`. The session header is in the OME-XML (MapAnnotation) and in
    TIFF tag 65401 of the first page; each frame header is in its OME `Plane` (with `DeltaT`
    and a MapAnnotation) and in TIFF tag 65400 of its own page. The OME-XML follows the OME
    2016-06 schema; the images are streamed, so recordings may exceed the memory.
  - New dependency: `tifffile`.
  - Example: `Examples/export_recordings.py` (`--demo` runs without a camera).

## [2.0.0] - 2026-10-28

### Changed

- **Breaking: `ImgData.data` is always three-dimensional, `(H, W, C)`.** A single channel (mono,
  raw Bayer) now comes as `(H, W, 1)` instead of `(H, W)`; RGB/BGR stays `(H, W, 3)`. Code no
  longer has to tell mono and colour cameras apart. The view is still zero-copy and read-only.
  To port code written for 1.x, take the channel: `img.data[..., 0]` is the former `(H, W)`
  view, again without a copy - e.g. for `img.data[y, x]` as a number or for
  `matplotlib.pyplot.imshow`, which rejects `(H, W, 1)`.

## [1.0.1] - 2026-10-27

### Fixed

- **The package failed to build without the MVS SDK.** If `External/hik/MVS` is missing, it now
  builds (CMake warns) with the simulated camera only. Importing it logs an ERROR saying so;
  `vendors()` leaves HikRobot out, and `create_camera(vendor="HikRobot")` raises
  `RuntimeError`. The CMake switch `HIK_MVS` (`AUTO` | `ON` | `OFF`) decides; `_hik.HAVE_MVS`
  tells which build is loaded.
- **The same for an SDK given by `MVS_ROOT`** (e.g. by a depending package through
  `config-settings-package`) that is missing or does not fit: a configure-time check compiles
  the SDK code and links against the library. The import ERROR names the reason
  (`_hik.MVS_MISSING_REASON`). Only `HIK_MVS=ON` makes the SDK mandatory.

## [1.0.0] - 2026-10-27

First versioned release. The earlier 0.x states have no changelog; the git history covers them.

### Added

- **Camera interface** (`Docs/UC2_cam_py_interface.md`): `create_camera`, `Camera`,
  `list_cameras`, `vendors`, the data classes `ImgData` and `ImgMetaData`.
- **Vendors:**
  - "HikRobot" through the MVS SDK, which ships with the package (`uc2_cam_py/_mvs`), so no
    installation under `/opt/MVS` is needed.
  - "Simulated" without hardware: a Gaussian spot walking over the sensor, on a constant offset
    and with shot noise.
- **Ring buffer** in shared memory, with zero-copy, read-only numpy views of the images.
- **Recording** to `.uc2rb` files with a `.json` sidecar; `store_image_data` switches it on and off
  at run time.
- **Camera process:** every camera runs in a process of its own (`uc2_cam_worker`,
  `separate_process=True` by default).
- **Live view** with `create_camera(..., debug_window=True)`, and the interactive
  `python -m uc2_cam_py`.
- **Logging:** the C++ log goes to Python `logging`, each camera to its own logger.
- **Versioning:** the file `VERSION` is the only source of the version number. The version is
  available as `uc2_cam_py.__version__`, `hik::version()` and `uc2_cam_worker --version`.
  Every recording stores it (`ImgMetaData.software_version`), and a camera process of another
  version is refused.
