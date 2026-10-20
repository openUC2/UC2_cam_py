"""Command-line entry point: ``python -m uc2_cam_py`` or the ``uc2_cam_py`` script.

Thin argument wrapper around :func:`uc2_cam_py.run_connection_test`, a quick
check that a HikRobot camera works. For acquisition use
:func:`uc2_cam_py.create_camera`.
"""

from __future__ import annotations

import argparse

import uc2_cam_py


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="uc2_cam_py",
        description="Run a connection test against a HikRobot / MVS camera.",
    )
    parser.add_argument("-i", "--index", type=int, default=-1,
                        help="device index (default: -1 = prompt)")
    parser.add_argument("-n", "--frames", type=int, default=0,
                        help="stop after N frames (default: 0 = until Enter)")
    parser.add_argument("-s", "--snapshot", default="",
                        help="save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif; "
                             "default: snapshot.png)")
    args = parser.parse_args()

    return uc2_cam_py.run_connection_test(index=args.index, frames=args.frames, snapshot=args.snapshot)


if __name__ == "__main__":
    raise SystemExit(main())
