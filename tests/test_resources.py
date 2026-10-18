"""Buffer size and disk-space checks (RAM / disk queries are mocked)."""

from __future__ import annotations

import pytest

from uc2_cam_py import _resources, defaults


def test_large_buffer_warns(open_camera, rec_logger, monkeypatch) -> None:
    monkeypatch.setattr(defaults, "BUFFER_WARN_BYTES", 1)
    open_camera()
    assert rec_logger.has("WARNING", "exceeds the warning threshold")


def test_buffer_larger_than_ram_fails(open_camera, rec_logger, monkeypatch) -> None:
    monkeypatch.setattr(_resources, "available_memory", lambda: 1000)
    with pytest.raises(MemoryError):
        open_camera()
    assert rec_logger.has("ERROR", "does not fit into the available RAM")


def test_unknown_ram_is_not_an_error(open_camera, monkeypatch) -> None:
    monkeypatch.setattr(_resources, "available_memory", lambda: None)
    assert open_camera().buffer_size > 0


def test_too_little_disk_space_fails(open_camera, rec_logger, monkeypatch) -> None:
    monkeypatch.setattr(_resources, "free_disk", lambda path: 10)
    with pytest.raises(OSError):
        open_camera(store_image_data=True)
    assert rec_logger.has("ERROR", "Not enough disk space")


def test_disk_space_is_only_checked_when_storing(open_camera, monkeypatch) -> None:
    monkeypatch.setattr(_resources, "free_disk", lambda path: 10)
    assert open_camera(store_image_data=False) is not None


def test_recording_time_estimate_is_logged(open_camera, rec_logger, monkeypatch) -> None:
    monkeypatch.setattr(_resources, "free_disk", lambda path: 10 * 2**30)
    open_camera(store_image_data=True)
    assert rec_logger.has("INFO", "Disk space for about")


def test_uncreatable_image_path_fails(open_camera, tmp_path) -> None:
    blocker = tmp_path / "a_file"
    blocker.write_text("not a directory")
    with pytest.raises(OSError):
        open_camera(store_image_data=True, image_file_path=blocker / "sub")


def test_real_queries_return_plausible_numbers(tmp_path) -> None:
    mem = _resources.available_memory()
    assert mem is None or mem > 0
    assert _resources.free_disk(tmp_path / "does" / "not" / "exist") > 0
