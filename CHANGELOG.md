# Changelog

All notable changes to `uc2_cam_py` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/) (see "Versioning" in the README).

## [Unreleased]

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
