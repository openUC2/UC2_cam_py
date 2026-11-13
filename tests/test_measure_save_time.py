"""Analysis/MeasureSaveTime.py: disk write time and readout time per image."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import hik_camera_connected

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "Analysis" / "MeasureSaveTime.py"
#: Faster than the simulated camera's default 10 images/s.
SIM = ("--simulated", "--frame-rate", "50")


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        check=False, capture_output=True, text=True, timeout=120, cwd=cwd,
    )


def check_report(result: subprocess.CompletedProcess[str], output: Path, images: int) -> None:
    assert result.returncode == 0, result.stderr
    for line in ("write per image", "readout per image", "frame interval", "close (fsync)",
                 "disk throughput", "camera data rate"):
        assert line in result.stdout
    assert output.exists()
    sidecar = json.loads(output.with_suffix(".json").read_text())
    assert sidecar["container"] == output.name             # renamed along with the file
    assert sidecar["writeStats"]["framesSaved"] >= images
    assert not list(output.parent.glob(".measure_*"))      # the work folder is gone


def test_the_script_measures_with_the_simulated_camera(tmp_path: Path) -> None:
    output = tmp_path / "data" / "Sample.uc2rb"
    check_report(run(*SIM, "--images", "20", "--output", str(output)), output, 20)


def test_the_default_target_is_analysis_in_the_working_directory(tmp_path: Path) -> None:
    result = run(*SIM, "--images", "5", cwd=tmp_path)
    check_report(result, tmp_path / "Analysis" / "SampleData.uc2rb", 5)


def test_a_second_run_replaces_the_recording(tmp_path: Path) -> None:
    output = tmp_path / "SampleData.uc2rb"
    check_report(run(*SIM, "--images", "5", "--output", str(output)), output, 5)
    second = run(*SIM, "--images", "8", "--output", str(output))
    check_report(second, output, 8)
    assert "replaced the previous SampleData.uc2rb" in second.stdout
    assert sorted(p.name for p in tmp_path.iterdir()) == ["SampleData.json", "SampleData.uc2rb"]


def test_too_few_images_are_refused(tmp_path: Path) -> None:
    result = run(*SIM, "--images", "1", "--output", str(tmp_path / "x.uc2rb"))
    assert result.returncode == 2   # argparse
    assert "at least 2" in result.stderr


def test_without_a_camera_the_script_says_so(tmp_path: Path) -> None:
    if hik_camera_connected():
        pytest.skip("a HikRobot camera is connected")
    result = run("--images", "5", "--output", str(tmp_path / "x.uc2rb"))
    assert result.returncode == 1
    assert "No HikRobot camera found" in result.stderr


@pytest.mark.hardware
def test_the_script_measures_with_the_hikrobot_camera(tmp_path: Path) -> None:
    if not hik_camera_connected():
        pytest.skip("no HikRobot camera connected")
    output = tmp_path / "SampleData.uc2rb"
    check_report(run("--images", "20", "--output", str(output)), output, 20)
