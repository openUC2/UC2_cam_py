"""The interactive entry point ``python -m uc2_cam_py`` (without a display)."""

from __future__ import annotations

import logging

import pytest

import uc2_cam_py
from uc2_cam_py import __main__ as cli


@pytest.fixture
def answers(monkeypatch) -> list[str]:
    """Lines that input() returns one by one (then EOF); the run stays headless."""
    lines: list[str] = []

    def fake_input(prompt: str = "") -> str:
        if not lines:
            raise EOFError
        return lines.pop(0)

    monkeypatch.setattr("builtins.input", fake_input)
    monkeypatch.setattr("sys.argv", ["uc2_cam_py"])
    monkeypatch.setattr(logging, "basicConfig", lambda **kwargs: None)   # leave pytest's logging alone
    monkeypatch.delenv("DISPLAY", raising=False)
    monkeypatch.delenv("WAYLAND_DISPLAY", raising=False)
    return lines


def test_lists_the_cameras_and_quits_on_an_empty_answer(answers, capsys) -> None:
    answers.append("")
    assert cli.main() == 0
    out = capsys.readouterr().out
    assert "Index" in out and "Vendor" in out and "Connection" in out
    assert "UC2-SIM" in out and "Simulated" in out and "SIM-0001" in out


def test_opens_the_chosen_camera_live_and_stops_on_enter(answers, capsys, monkeypatch) -> None:
    index = next(i for i, c in enumerate(uc2_cam_py.list_cameras()) if c.vendor == "Simulated")
    opened = []
    create = uc2_cam_py.create_camera

    def spy(**kwargs):
        cam = create(**kwargs)
        opened.append((cam, kwargs))
        return cam

    monkeypatch.setattr(uc2_cam_py, "create_camera", spy)
    answers += ["x", "99", str(index), ""]   # two invalid answers, the choice, then Enter
    assert cli.main() == 0

    cam, kwargs = opened[0]
    assert kwargs["vendor"] == "Simulated" and kwargs["serial"] == "SIM-0001"
    assert kwargs["store_image_data"] is False
    assert kwargs["debug_window"] is True and kwargs["start_on_init"] is True
    assert cam._closed                        # closed on the way out
    assert "Please enter a number from 0 to" in capsys.readouterr().out
