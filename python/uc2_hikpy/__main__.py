"""Command-line entry point: `python -m uc2_hikpy` or the `uc2_hikpy` script.

Thin argument wrapper around :func:`uc2_hikpy.run`.
"""

from __future__ import annotations

import argparse

import uc2_hikpy


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="uc2_hikpy",
        description="Grab images from a HikRobot / MVS camera.",
    )
    parser.add_argument("-i", "--index", type=int, default=-1,
                        help="device index (default: -1 = prompt)")
    parser.add_argument("-n", "--frames", type=int, default=0,
                        help="stop after N frames (default: 0 = until Enter / window close)")
    parser.add_argument("-s", "--snapshot", default="",
                        help="save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif)")
    parser.add_argument("--no-display", action="store_true",
                        help="do not open a preview window (headless)")
    args = parser.parse_args()

    return uc2_hikpy.run(
        index=args.index,
        frames=args.frames,
        snapshot=args.snapshot,
        display=not args.no_display,
    )


if __name__ == "__main__":
    raise SystemExit(main())
