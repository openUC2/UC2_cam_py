# UC2_cam_py — Camera Interface

Specification of the Python camera interface of `uc2_cam_py` (version 0.2.0). It is the
consolidated form of the original interface draft: names are consistently `snake_case`,
contradictions and gaps of the draft are resolved (see [§10](#10-changes-against-the-draft)),
and everything marked *addition* goes beyond the draft.

```python
import uc2_cam_py
from uc2_cam_py import ImgData, ImgMetaData

with uc2_cam_py.create_camera(vendor="HikRobot", connection_type="USB3",
                              start_on_init=True) as cam:
    cam.exposure_time_ms = 5.0
    img: ImgData | None = cam.get_last_image()
    if img is not None:
        print(img.data.shape, img.data.dtype, img.time_ns, img.is_valid())
    meta: ImgMetaData | None = cam.get_image_info()
```

## Contents

1. [Concepts](#1-concepts)
2. [Initialisation: `create_camera`](#2-initialisation-create_camera)
3. [Properties (read-only)](#3-properties-read-only)
4. [Settings (read/write)](#4-settings-readwrite)
5. [Methods](#5-methods)
6. [Data classes](#6-data-classes)
7. [Behaviour in detail](#7-behaviour-in-detail)
8. [Vendors](#8-vendors)
9. [Helpers](#9-helpers)
10. [Changes against the draft](#10-changes-against-the-draft)
11. [Known limitations](#11-known-limitations)

---

## 1. Concepts

- **Vendor-neutral.** `create_camera(vendor=...)` returns a `Camera`. Vendor-specific details
  (firmware units, node names, model data) live in the vendor classes `HikRobotCamera` and
  `SimulatedCamera`.
- **Ring buffer.** Images are acquired into a ring buffer of `buffer_size` slots
  (C++ `UC2_RingBuff`). The camera thread copies each image once into a slot; nothing is
  copied on the way out.
- **Zero-copy access.** `ImgData.data` is a read-only numpy view onto the slot.
- **Recording.** With `store_image_data=True` every image is also streamed to a `.uc2rb`
  file (plus a `.json` sidecar) in `image_file_path`, by a writer thread that never blocks the
  camera.
- **Units at the interface:** exposure in **ms**, frame rate in **1/s**, gain in **dB**,
  pixel pitch/size in **µm**, ROI and pixel count in **unbinned sensor pixels**, timestamps in
  **ns** (monotonic clock). Vendors convert from their firmware units.

## 2. Initialisation: `create_camera`

```python
create_camera(vendor=None, connection_type=None, serial=None, index=None, buffer_size=None,
              start_on_init=False, image_file_path=None, store_image_data=True, logger=None,
              **vendor_options) -> Camera
```

| Parameter | Type | Default | Description |
|---|---|---|---|
| `vendor` | `str \| None` | `None` → `defaults.VENDOR` (`"HikRobot"`) | `"HikRobot"` or `"Simulated"`, case-insensitive. |
| `connection_type` | `str \| None` | `None` (any) | `"USB3"` or `"GigE"` (GigE Vision), case-insensitive. Restricts the device search. |
| `serial` | `str \| None` | `None` | Serial number of the camera to open. |
| `index` | `int ≥ 0 \| None` | `None` | Index among the cameras of `connection_type` (order of `list_devices()`). |
| `buffer_size` | `int ≥ 2 \| None` | `None` → `defaults.BUFFER_SIZE` (16) | Number of images (slots) in the ring buffer. |
| `start_on_init` | `bool` | `False` | Start acquisition immediately after initialisation. |
| `image_file_path` | `str \| PathLike \| None` | `None` → `defaults.IMAGE_FILE_PATH` (`~/UC2_recordings`) | Directory for the recorded data; created if missing. |
| `store_image_data` | `bool` | `True` | Write every image to disk (in `image_file_path`). |
| `logger` | logger-like `\| None` | `None` → `logging.getLogger("uc2_cam_py")` | Object with `error`, `warning` (or `warn`), `info`, `debug` methods. |
| `**vendor_options` | | | *Addition.* Vendor-specific extras, see [§8](#8-vendors). |

**Camera selection:**
- `serial` given: that camera.
- `index` given: that camera.
- Neither given: the first available camera.
- Both given: `ValueError`.

**Initialisation does not change camera settings.** Exposure, gain, frame rate, binning and ROI
keep their current values. The camera is put into free-run mode (no trigger).

**Logged at INFO on success:**
- vendor, model, serial, connection type and index,
- sensor size, pixel pitch, image size, pixel format and binning,
- buffer size in slots and MiB, recording path,
- which settings the firmware allows.

**Resource checks** (at init and at every `start()`):

| Condition | Result |
|---|---|
| Buffer (`buffer_size` × image bytes) > `defaults.BUFFER_WARN_BYTES` (2 GiB) | WARNING |
| Buffer > available RAM | ERROR + `MemoryError` |
| `store_image_data` and `image_file_path` cannot be created | ERROR + `OSError` |
| `store_image_data` and free disk space < one full buffer | ERROR + `OSError(ENOSPC)` |
| `store_image_data` (otherwise) | INFO with the estimated recording time until the disk is full |

**Errors:**

| Exception | Raised for |
|---|---|
| `TypeError` | an argument of the wrong type, or a logger without the required methods |
| `ValueError` | an unknown vendor or connection type, `serial` and `index` together, `buffer_size < 2`, `index < 0` |
| `IndexError` | an `index` beyond the connected cameras |
| `RuntimeError` | no matching camera, a camera that cannot be opened (e.g. in use), or an unknown pixel count |
| `MemoryError` / `OSError` | failed resource checks (see above) |

## 3. Properties (read-only)

| Property | Type | Description |
|---|---|---|
| `can_set_gain` | `bool` | Gain can be set in the firmware. |
| `can_set_frame_rate` | `bool` | Frame rate can be set in the firmware. |
| `can_set_binning` | `bool` | Binning can be set in the firmware. |
| `can_set_trigger` | `bool` | The camera has a configurable trigger (not used yet, see `trigger`). |
| `can_set_exposure` | `bool` | Exposure time can be set in the firmware. |
| `can_set_roi` | `bool` | *Addition.* A region of interest can be set. |
| `range_exposure_time_ms` | `(float ≥ 0, float > 0) \| None` | (min, max) exposure time in ms. `None` if not settable or no range is given. |
| `range_frame_rate_per_sec` | `(float ≥ 0, float > 0) \| None` | (min, max) frame rate in 1/s. `None` if not settable. |
| `range_gain_db` | `(float ≥ 0, float > 0) \| None` | (min, max) gain in dB. `None` if not settable. |
| `range_binning` | `list[int]` | Allowed symmetric binning factors, a subset of `[1, 2, 4]`. `[1]` if binning is not possible. |
| `pixel_count` | `(int > 0, int > 0)` | Full **unbinned** sensor resolution (x, y). Read from the firmware, otherwise taken from the vendor's model table; if neither knows it, initialisation fails with `RuntimeError`. |
| `pixel_pitch_um` | `(float > 0, float > 0) \| None` | Centre-to-centre pixel distance (x, y) in µm. `None` if neither the firmware nor the model table provides it. |
| `pixel_size_um` | `(float > 0, float > 0) \| None` | Photosensitive width per pixel (x, y) in µm. Falls back to `pixel_pitch_um` if unknown. |
| `connection_type` | `str` | `"USB3"` or `"GigE"` of the opened camera. |
| `vendor` | `str` | `"HikRobot"` or `"Simulated"`. |
| `serial` | `str` | Serial number of the opened camera. |
| `index` | `int ≥ 0` | Index of the opened camera among the cameras of its connection type. |
| `buffer_size` | `int` | Number of slots in the image buffer. |
| `is_recording` | `bool` | `True` while images are acquired into the buffer (and to disk if enabled), i.e. between `start()` and `stop()`. |
| `model` | `str` | *Addition.* Camera model. |
| `pixel_format` | `str` | *Addition.* E.g. `"Mono8"`, `"Mono12"`, `"BayerRG8"`, `"RGB8"`. |
| `image_size` | `(int, int)` | *Addition.* Current image (width, height) after ROI and binning. |
| `recording_file` | `str \| None` | *Addition.* Path of the current or last `.uc2rb` file. |

Capabilities and ranges are evaluated **live**. Exposure, gain and frame rate count as settable
even while the firmware's auto mode is active, because writing a value switches auto off.

## 4. Settings (read/write)

| Setting | Type | Default | Description | If not settable |
|---|---|---|---|---|
| `exposure_time_ms` | `float > 0 \| None` | camera's value | Integration time in ms. Writing `None` switches to automatic exposure. | WARNING |
| `frame_rate_per_sec` | `float > 0 \| None` | camera's value | Frame rate in 1/s. Writing `None` removes the limit, so the camera runs at its maximum rate. | WARNING |
| `gain_db` | `float ≥ 0 \| None` | camera's value | Analog gain in dB. Writing `None` switches to automatic gain. | WARNING |
| `binning` | `1 \| 2 \| 4` | camera's value (usually 1) | Symmetric hardware binning. `None` means 1. | WARNING |
| `trigger` | `None` | `None` | Placeholder; the camera always runs in free-run mode. Writing anything else changes nothing. | ERROR |
| `roi` | `(x, y, width, height) \| None` | `None` | Region of interest in **unbinned sensor pixels**, x, y ≥ 0, width, height > 0. `None` is the complete sensor. | WARNING |

**Reading** returns the camera's actual value; `None` only if it cannot be read. For example,
`exposure_time_ms` returns the current value even in auto mode. `frame_rate_per_sec` without a
limit returns the rate the camera actually reaches. `roi` returns `None` when the complete
sensor is read out.

**Clamping** (discrete or bounded values):
- Floats are clamped to their range, logged at INFO. The firmware may round further; the
  applied value is read back and logged at INFO if it differs.
- Binning snaps to the nearest value of `range_binning`; a tie goes to the smaller value.
- The ROI is clamped to the sensor and snapped to the nearest point of the camera's pixel grid.

**Invalid values** raise exceptions:
- the wrong type (e.g. a `str`, or `bool` for a number) → `TypeError`,
- values violating the type constraint (`exposure ≤ 0`, `gain < 0`, `binning < 1`, ROI
  width ≤ 0) → `ValueError`.

**A refused write.** If the camera rejects a write although it reports the setting as settable,
`RuntimeError` is raised and the reason is logged.

## 5. Methods

| Method | Parameters | Returns | Description |
|---|---|---|---|
| `get_last_image()` | – | `ImgData \| None` | The most recently recorded image; `None` if no image has been recorded yet. |
| `get_image(index)` | `int`, `0 ≤ index < buffer_size` | `ImgData` | The image in buffer slot `index`. `TypeError` for a non-int, `IndexError` if out of range, `LookupError` if the slot holds no completely recorded image. |
| `get_image_info()` | – | `ImgMetaData \| None` | Metadata of the current or last recording **session** (the `UC2_SessionHeader`); `None` before the first `start()`. |
| `start()` | – | `None` | Start acquisition: allocates a new buffer for the current geometry and opens a new session (a new `.uc2rb` file if storing). Does nothing if already recording. Raises `RuntimeError` on failure, plus `MemoryError`/`OSError` from the resource checks. |
| `stop()` | – | `None` | Stop acquisition. Recorded images stay readable until the next `start()`. |
| `close()` | – | `None` | *Addition.* Stop and release the camera. Images already read stay valid. Afterwards `start()` raises `RuntimeError`. |
| `with ... as cam` | – | – | *Addition.* `__enter__`/`__exit__`; `__exit__` calls `close()`. |

**Slot order.** Slots are filled cyclically, `slot = sequence % buffer_size`, so the slot order is
not the time order. `ImgData.sequence` gives the order.

## 6. Data classes

Both are public `@dataclass`es (`from uc2_cam_py import ImgData, ImgMetaData`).

### `ImgData`

| Field | Type | Description |
|---|---|---|
| `data` | `numpy.ndarray` | Read-only zero-copy view onto the ring slot. `(H, W)` for mono/raw Bayer, `(H, W, 3)` for RGB/BGR; `uint8` or `uint16`. |
| `time_before_ns` | `int` | Monotonic ns when the image reached the host (camera callback). |
| `time_after_ns` | `int` | Monotonic ns when the copy into the slot was complete. |
| `time_ns` | `int` | Midpoint `(time_before_ns + time_after_ns) // 2`. |
| `sequence` | `int` | *Addition.* Frame index within the session (0, 1, 2, ...). |
| `slot` | `int` | *Addition.* Buffer slot (`sequence % buffer_size`). |

| Method | Description |
|---|---|
| `is_valid() -> bool` | *Addition.* `False` if the camera has overwritten the slot since it was read (see [§7.4](#74-zero-copy-access-and-validity)). Always `True` for user-constructed objects. |

### `ImgMetaData` (frozen)

All fields of `UC2_SessionHeader`; the same header starts every `.uc2rb` file.

| Field | Type | Description |
|---|---|---|
| `magic` | `str` | `"UC2RB"` |
| `format_version` | `int` | Container format version |
| `header_bytes` | `int` | Size of the session header (8448) |
| `width`, `height` | `int` | Image size after ROI and binning |
| `channels` | `int` | 1 (mono, raw Bayer) or 3 (RGB/BGR) |
| `depth` | `int` | Bytes per sample: 1 = `uint8`, 2 = `uint16` |
| `frame_bytes` | `int` | Bytes per image |
| `slot_count` | `int` | Number of buffer slots |
| `layout_version` | `int` | Shared-memory layout version |
| `start_wall_ns` | `int` | UTC epoch ns at the session start |
| `start_mono_ns` | `int` | Monotonic ns at the same instant |
| `slot_stride` | `int` | Bytes between slot payloads |
| `camera_id` | `str` | `"<model>-<serial>"` |
| `comment` | `str` | JSON with the camera settings at the session start (vendor, model, serial, exposure, gain, frame rate, binning, ROI, pixel pitch, ...) |

| Member | Description |
|---|---|
| `dtype` (property) | numpy dtype name of the samples. |
| `wall_time_ns(monotonic_ns)` | UTC epoch ns of a monotonic timestamp of this session, e.g. `meta.wall_time_ns(img.time_ns)`. |

## 7. Behaviour in detail

### 7.1 `None` semantics
- **At initialisation:** nothing is written to the camera.
- **Reading a setting:** returns `None` only when the value cannot be read (or for `roi`, when
  the complete sensor is used).
- **Writing `None`:** means "let the camera decide":
  - exposure and gain: automatic mode,
  - frame rate: no limit,
  - ROI: complete sensor,
  - binning: 1,
  - trigger: free-run.

### 7.2 ROI and binning
- **Coordinates.** The ROI is always given in unbinned sensor pixels, relative to `pixel_count`.
  The vendor converts to its firmware coordinates; HikRobot counts binned pixels.
- **Restart while recording.** ROI and binning change the image size, and the ring buffer cannot
  change its geometry. A change **while recording** therefore stops the acquisition, applies the
  change and starts again. The new geometry gets a new buffer and a new session (a new `.uc2rb`
  file). This is logged at INFO.
- **While stopped,** the change applies from the next `start()`.

### 7.3 Timestamps
- **Clock.** All times are monotonic ns (`time.monotonic_ns()`, `steady_clock`): immune to
  NTP and daylight-saving jumps.
- **Absolute time.** `ImgMetaData.wall_time_ns()` converts to UTC via the session anchors
  `start_wall_ns`/`start_mono_ns`.
- **What the two times mean.** `time_before_ns` marks the arrival of the image at the host,
  **not** the start of the exposure. `time_after_ns` marks the end of the copy into the slot.

### 7.4 Zero-copy access and validity
- **No copy.** `ImgData.data` aliases the ring slot.
- **Lifetime.** The view keeps the buffer memory alive, even after `close()` or after the
  camera object is gone, so reading it can never crash.
- **Read-only.** Writing into the view raises `ValueError`, because the slot also feeds the disk
  stream.
- **Overwriting.** The buffer runs in *overwrite* mode: the camera never waits for readers. A
  slot can therefore be overwritten while Python still reads it. To get a consistent result:
  - Process the image, then call `is_valid()`. `True` guarantees the data was consistent.
  - If you need the image for longer, take `img.data.copy()`.
  - Alternatively, use a `buffer_size` large enough for the processing time.

### 7.5 Recording
- **Files.** Each session is one append-only file
  `<image_file_path>/uc2_<camera_id>_<YYYYmmdd_HHMMSS>[_NNN].uc2rb`, laid out as
  `[session header 8448 B]` followed by `[frame header 64 B][payload]` per image, plus a `.json`
  sidecar with the metadata.
- **Writer.** A writer thread reads the images from the buffer and writes them with `writev()`.
  The camera thread never touches the file system.
- **Dropped frames.** If the disk cannot keep up, frames are dropped from the file (counted),
  never from the camera.
- Format details: [RingBuff_REF.md](RingBuff_REF.md) §8.

### 7.6 Logging
- **Which logger.** Python-side messages of a camera go to its `logger`. The C++ core's messages
  (SDK, ring buffer, disk writer) are forwarded to the logger of the most recently created
  camera, or to `logging.getLogger("uc2_cam_py")`.
- **Levels.** C++ levels map to `error` / `warning` / `info` / `debug`.
- **GIL.** C++ threads never wait for the GIL. They put messages into a queue, and a Python
  daemon thread delivers them with the GIL held. A busy Python thread therefore cannot stall the
  camera, and no deadlock between the GIL and C++ locks is possible.
- **Delay.** Forwarded messages arrive asynchronously, within a few ms.

### 7.7 Threading
- **Callers.** Camera methods may be called from several Python threads.
- **GIL release.** `start`, `stop`, `close` and ROI/binning changes release the GIL while they
  wait.
- **Acquisition.** The image copy runs on a C++ thread without the GIL.

### 7.8 Error policy

| Situation | Reaction |
|---|---|
| A wrong argument type | `TypeError` |
| A value outside the type constraint | `ValueError` |
| An index out of range | `IndexError` |
| An empty buffer slot | `LookupError` |
| A wrong state (closed camera, start failed) | `RuntimeError` |
| The setting is not supported by the camera | **no exception**, log entry per [§4](#4-settings-readwrite) |
| A value outside the camera's range | clamped, INFO |

## 8. Vendors

### HikRobot (`HikRobotCamera`, MVS SDK)
- **Firmware nodes and units:**
  - `ExposureTime` in µs, converted to ms,
  - `Gain` in dB,
  - `AcquisitionFrameRate` / `AcquisitionFrameRateEnable` in 1/s,
  - `BinningHorizontal`/`BinningVertical`,
  - `Width`/`Height`/`OffsetX`/`OffsetY` in binned pixels,
  - `SensorWidth`/`SensorHeight`, else `WidthMax`/`HeightMax` × binning.
- **Pixel formats:**

  | Format | Stored as |
  |---|---|
  | Mono8, Bayer\*8 | `uint8 (H, W)` |
  | Mono10/12/16, Bayer\*10/12/16 (unpacked) | `uint16 (H, W)`, values not rescaled |
  | RGB8/BGR8 | `uint8 (H, W, 3)` |
  | Packed formats (Mono12Packed, Bayer\*Packed), YUV | switched to the unpacked equivalent at initialisation (INFO); `RuntimeError` if the camera has none |

- **Model table.** `uc2_cam_py.hikrobot.MODELS` holds pixel pitch, size and count per model,
  for what the firmware does not report. It is **empty** for now: add models only with values
  from their datasheet.
- **Device list.** `uc2_cam_py.list_devices(connection_type=None)` lists the connected cameras,
  e.g. to find serial numbers.

### Simulated (`SimulatedCamera`)
- **Purpose.** A camera without hardware, for tests and development, running through the same
  buffer path.
- **Image.** A Gaussian spot walks row by row over a 5 × 4 grid of the sensor.
  `cam.spot_position(sequence)` returns its exact centre for every image.
- **Settable:** exposure, gain, frame rate, binning {1, 2, 4} and ROI. **Not settable:**
  trigger. Exposure and gain do not change the image.
- **Fixed values:** pixel pitch 5.86 µm, serial `SIM-0001` (or the given serial), index 0.
- **Vendor options:**
  - `pixel_format` (`"Mono16"` default, or `"Mono8"`),
  - `sensor_width`, `sensor_height` (1920 × 1200),
  - `frame_rate` (10).

## 9. Helpers

| Name | Description |
|---|---|
| `uc2_cam_py.list_devices(connection_type=None)` | Connected HikRobot cameras (`DeviceInfo`: transport, model_name, serial, user_name, ip). |
| `uc2_cam_py.run_connection_test(...)` / `python -m uc2_cam_py` | Quick diagnostic of a HikRobot camera (grab, preview, snapshot). |
| `uc2_cam_py.preview.PreviewWindow` | Simple live view: `win.show(cam.get_last_image())` until the window is closed (X11, no extra dependency). |
| `uc2_cam_py.defaults` | Default values (vendor, buffer size, warning threshold, recording path); to be overridden by `config.yaml` later. |
| `uc2_cam_py.vendors()` | Available vendor names. |

`uc2_cam_py._hik` is the compiled low-level module and is not part of this interface.

## 10. Changes against the draft

**Names (draft → Python):**

| Draft | Python |
|---|---|
| `canSetGain`, `canSetFrameRate`, `canSetBinning`, `canSetTrigger`, `canSetExposure` | `can_set_gain`, `can_set_frame_rate`, `can_set_binning`, `can_set_trigger`, `can_set_exposure` |
| `rangeExposure`, `rangeFrameRate`, `rangeGain`, `rangeBinning` | `range_exposure_time_ms`, `range_frame_rate_per_sec`, `range_gain_db`, `range_binning` |
| `pixelcount`, `pixelpitch_um`, `pixelsize_um` | `pixel_count`, `pixel_pitch_um`, `pixel_size_um` |
| `bufferSize`, `isRecording` | `buffer_size`, `is_recording` |
| `framerate_per_sec`, `ROI` | `frame_rate_per_sec`, `roi` |
| init `startOnInit`, `ImageFilePath`, `storeImageData`, Image BufferSize | `start_on_init`, `image_file_path`, `store_image_data`, `buffer_size` |
| `imgData`: `data`, `timeBefore`, `timeAfter`, `time` | `ImgData`: `data`, `time_before_ns`, `time_after_ns`, `time_ns` |
| `imgMetaData` | `ImgMetaData` |

**Corrections:**
- `time` was `(after − before)/2`, i.e. half the duration; it is now the midpoint `(before + after) // 2`.
- Timestamps are `int` nanoseconds. A float loses ns precision after about 104 days of uptime.
- The ROI has offsets: `(x, y, width, height)`, not only a size.
- `frame_rate_per_sec` depends on `can_set_frame_rate` (the draft said `canSetExposure`).
- The gain unit is dB (the draft said Hz).
- `range_gain_db` is `None` if the gain is not settable (the draft said "exposure time").
- `buffer_size` counts slots (images).
- The buffer size check is against RAM, the disk check against one full buffer.
- `exposure_time_ms` and `frame_rate_per_sec` log a WARNING when not settable (the draft left the
  level open).

**Additions:**
- `close()` and the context manager,
- `ImgData.sequence`, `ImgData.slot`, `ImgData.is_valid()`,
- `ImgMetaData.dtype`, `ImgMetaData.wall_time_ns()`,
- `can_set_roi`, `model`, `pixel_format`, `image_size`, `recording_file`,
- `vendor_options`, `list_devices`, the `preview` helper,
- the settings JSON in the session comment.

## 11. Known limitations

- **Trigger.** Only a placeholder; the camera always runs in free-run mode.
- **Exposure timestamp.** The camera's device timestamp (start of exposure) is not recorded
  yet; `time_before_ns` is the arrival at the host.
- **Logging with several cameras.** C++ messages have no camera reference; with several cameras
  they go to the most recently created camera's logger.
- **Pixel pitch.** The HikRobot model table is empty, so `pixel_pitch_um` is `None` until
  entries are added.
- **Untested on hardware.** No HikRobot camera was available during development. Verify these
  with `pytest -m hardware` on a real camera:
  - node names and units,
  - binning enumeration values (assumed to equal the factor),
  - `SensorWidth`/`SensorHeight`,
  - the pixel-format switch,
  - serial selection.
