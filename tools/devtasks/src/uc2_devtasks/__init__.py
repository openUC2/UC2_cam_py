"""Developer commands for uc2_cam_py: ``uv run py_tests | c_tests | all_tests``.

Each command returns the exit code of the tool it runs, so ``uv run ...``
fails exactly when the tests fail. ``uv run main`` needs no code here: it is
the entry point ``uc2_cam_py.__main__:main`` (see ../../pyproject.toml).
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

#: Project root. The package is installed editable, so this file stays at
#: <root>/tools/devtasks/src/uc2_devtasks/__init__.py.
ROOT = Path(__file__).resolve().parents[4]
#: CMake preset whose tree c_tests builds and tests (the default, with the Python module).
PRESET = "debug"


def _root() -> Path:
    if not (ROOT / "pyproject.toml").is_file() or not (ROOT / "CMakePresets.json").is_file():
        raise SystemExit(f"uc2_devtasks: no uc2_cam_py project at {ROOT} - install it editable from tools/devtasks")
    return ROOT


def _run(cmd: list[str]) -> int:
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=_root())


def _cached(cache: Path, key: str, fallback: str) -> str:
    """Value of ``key`` in a CMakeCache.txt (``KEY:TYPE=value``), else ``fallback``."""
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    return fallback


def _py_tests(args: list[str]) -> int:
    return _run([sys.executable, "-m", "pytest", *args])


def _c_tests(args: list[str]) -> int:
    cache = _root() / "build" / PRESET / "CMakeCache.txt"
    if not cache.is_file():
        rc = _run(["cmake", "--preset", PRESET])
        if rc:
            return rc
    # Build with the cmake that configured the tree: under `uv run` the venv's cmake
    # comes first on PATH and may be a different version.
    cmake = _cached(cache, "CMAKE_COMMAND", "cmake")
    ctest = _cached(cache, "CMAKE_CTEST_COMMAND", "ctest")
    rc = _run([cmake, "--build", "--preset", PRESET])
    if rc:
        return rc
    return _run([ctest, "--preset", PRESET, "--output-on-failure", *args])


def py_tests() -> int:
    """``uv run py_tests [pytest arguments]``: the pytest suite."""
    return _py_tests(sys.argv[1:])


def c_tests() -> int:
    """``uv run c_tests [ctest arguments]``: build the debug preset, then run CTest."""
    return _c_tests(sys.argv[1:])


def all_tests() -> int:
    """``uv run all_tests``: C++ and Python tests; both always run."""
    if sys.argv[1:]:
        raise SystemExit("all_tests takes no arguments - use c_tests / py_tests to pass some")
    results = {"C++": _c_tests([]), "Python": _py_tests([])}
    summary = ", ".join(f"{name} {'passed' if rc == 0 else f'FAILED (exit {rc})'}" for name, rc in results.items())
    print(f"\nall_tests: {summary}")
    return 0 if not any(results.values()) else 1
