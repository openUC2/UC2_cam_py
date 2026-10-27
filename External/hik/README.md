# HikRobot MVS SDK (not in git)

uc2_cam_py builds and runs against the MVS SDK in `External/hik/MVS/`, not against an
installation in `/opt/MVS`. The SDK is proprietary, so neither the packages nor the extracted
files are committed (see `.gitignore`).

The SDK is optional: without `MVS/` - or with an SDK that does not fit, also one given by
`-DMVS_ROOT` - the project still builds (CMake warns), but without HikRobot support: only the
simulated camera is available, and importing `uc2_cam_py` logs an ERROR saying so and why.
`-DHIK_MVS=ON` turns a missing or unfit SDK into a build error instead. Setting the SDK up later and building again (reinstalling the Python
package) adds HikRobot support; a CMake build tree notices the new `MVS/` and re-configures
by itself.

## Setting it up

1. Download the Linux packages from HikRobot and put the zips here, unchanged, e.g.

   ```
   External/hik/MVS-5.1.1_Linux_x86_64_20261016.zip
   External/hik/MVS-5.1.1_Linux_aarch64_20261016.zip
   ```

2. Run `uv run fetch_mvs` in the project root. It extracts only what the build and the
   runtime need into `External/hik/MVS/`:

   ```
   MVS/include/                  headers
   MVS/lib/64/                   x86_64 runtime libraries (with ThirdParty/)
   MVS/lib/aarch64/              aarch64 runtime libraries
   MVS/lib/CLProtocol/<arch>/    GenICam CLProtocol per architecture
   MVS/license/                  HikRobot's licence notices
   MVS/scripts/                  HikRobot's USB/GigE system setup scripts (see below)
   MVS/VERSION                   package and SDK version
   ```

   Running it again replaces `MVS/` with the newest package found here.

3. Build as usual (`uv sync`, `uv run c_tests`, ...). The libraries are copied into the
   installed package (`uc2_cam_py/_mvs/`), so nothing points back to this folder at runtime.

## What still needs root, once per machine

The SDK files cannot configure the operating system. HikRobot's scripts for that are in
`MVS/scripts/` - read them before running them with `sudo`:

- **USB3 cameras:** `set_usb_priority.sh` (udev rule so a normal user may open the camera) and
  `set_usbfs_memory_size.sh` (USB buffer memory for high frame rates).
- **GigE cameras:** `set_rp_filter.sh` and `set_socket_buffer_size.sh` (recommended).

The Windows package (`MVS_Win_STD_*.zip`) can live here too; it is not used yet.

Do not publish wheels that contain these libraries before clarifying HikRobot's licence terms.
