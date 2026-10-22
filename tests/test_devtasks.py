"""The developer commands ``uv run main | py_tests | c_tests | all_tests`` (tools/devtasks)."""

from __future__ import annotations

from importlib.metadata import entry_points
from pathlib import Path

import uc2_devtasks


def test_the_commands_are_registered() -> None:
    scripts = {ep.name: ep.value for ep in entry_points(group="console_scripts")}
    assert scripts["main"] == "uc2_cam_py.__main__:main"
    for name in ("py_tests", "c_tests", "all_tests"):
        assert scripts[name] == f"uc2_devtasks:{name}"


def test_the_project_root_is_found() -> None:
    assert uc2_devtasks._root() == Path(__file__).resolve().parents[1]


def test_cmake_cache_entries_are_read(tmp_path) -> None:
    cache = tmp_path / "CMakeCache.txt"
    cache.write_text("// the cmake that configured the tree\nCMAKE_COMMAND:INTERNAL=/opt/cmake/bin/cmake\n")
    assert uc2_devtasks._cached(cache, "CMAKE_COMMAND", "cmake") == "/opt/cmake/bin/cmake"
    assert uc2_devtasks._cached(cache, "CMAKE_CTEST_COMMAND", "ctest") == "ctest"
