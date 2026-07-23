"""Very short test program: start HikCamtest from Python.

Run it with:  uv run python tests/test_start.py
"""

import hikcamtest


def main() -> int:
    print(f"hikcamtest {hikcamtest.__version__}")
    print(f"Got file from {hikcamtest.__file__}")
    devices = hikcamtest.list_devices()
    print(f"Found {len(devices)} camera(s):")
    for i, dev in enumerate(devices):
        print(f"  [{i}] {dev!r}")

    if not devices:
        print("No cameras connected - nothing to start.")
        return 0

    # Start HikCamtest on the first camera: grab a few frames, show the preview
    # window (if available) and save the first frame to snapshot.png.
    return hikcamtest.run(index=0, frames=50, snapshot="snapshot.png", display=True)


if __name__ == "__main__":
    raise SystemExit(main())
