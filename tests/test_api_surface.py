"""The public API matches the interface specification (Docs/UC2_cam_py_interface.md)."""

from __future__ import annotations

import dataclasses
import importlib
import inspect

import numpy as np
import pytest

import uc2_cam_py
from uc2_cam_py import Camera, ImgData, ImgMetaData

READ_ONLY = [
    "can_set_gain", "can_set_frame_rate", "can_set_binning", "can_set_trigger", "can_set_exposure",
    "range_exposure_time_ms", "range_frame_rate_per_sec", "range_gain_db", "range_binning",
    "pixel_count", "pixel_pitch_um", "pixel_size_um", "connection_type", "vendor", "serial",
    "index", "buffer_size", "is_recording", "process_id",
]
SETTINGS = ["exposure_time_ms", "frame_rate_per_sec", "gain_db", "binning", "trigger", "roi"]
METHODS = ["get_last_image", "get_image", "get_image_info", "start", "stop", "close"]


def test_data_classes_are_importable_from_outside() -> None:
    from uc2_cam_py import ImgData as A, ImgMetaData as B  # noqa: N814 - the point of the test
    from uc2_cam_py.data import ImgData as A2, ImgMetaData as B2

    assert A is A2 and B is B2
    assert dataclasses.is_dataclass(ImgData)
    assert dataclasses.is_dataclass(ImgMetaData)
    assert "ImgData" in uc2_cam_py.__all__ and "ImgMetaData" in uc2_cam_py.__all__


def test_img_data_fields_and_user_construction() -> None:
    names = [f.name for f in dataclasses.fields(ImgData) if not f.name.startswith("_")]
    assert names == ["data", "time_before_ns", "time_after_ns", "time_ns", "sequence", "slot"]

    img = ImgData(data=np.zeros((2, 3), np.uint16), time_before_ns=10, time_after_ns=20,
                  time_ns=15, sequence=0, slot=0)
    assert img.is_valid()   # not tied to a camera -> always valid
    assert isinstance(img, ImgData)


def test_img_meta_data_is_frozen_and_complete() -> None:
    names = [f.name for f in dataclasses.fields(ImgMetaData)]
    assert names == [
        "magic", "format_version", "header_bytes", "width", "height", "channels", "depth",
        "frame_bytes", "slot_count", "layout_version", "start_wall_ns", "start_mono_ns",
        "slot_stride", "camera_id", "comment", "software_version",
    ]
    meta = ImgMetaData(magic="UC2RB", format_version=1, header_bytes=8448, width=4, height=2,
                       channels=1, depth=2, frame_bytes=16, slot_count=2, layout_version=1,
                       start_wall_ns=1_000, start_mono_ns=100, slot_stride=64, camera_id="X-1",
                       comment="{}", software_version="1.0.0")
    assert meta.dtype == "uint16"
    assert meta.wall_time_ns(150) == 1_050
    assert dataclasses.asdict(meta)["camera_id"] == "X-1"
    with pytest.raises(dataclasses.FrozenInstanceError):
        meta.width = 5   # type: ignore[misc]


@pytest.mark.parametrize("name", READ_ONLY)
def test_read_only_properties(name: str) -> None:
    attr = getattr(Camera, name)
    assert isinstance(attr, property), name
    assert attr.fset is None, f"{name} must be read-only"


@pytest.mark.parametrize("name", SETTINGS)
def test_settings_are_readable_and_writable(name: str) -> None:
    attr = getattr(Camera, name)
    assert isinstance(attr, property), name
    assert attr.fset is not None, f"{name} must be writable"


@pytest.mark.parametrize("name", METHODS)
def test_methods_exist(name: str) -> None:
    assert callable(getattr(Camera, name))
    assert hasattr(Camera, "__enter__") and hasattr(Camera, "__exit__")


def test_create_camera_signature() -> None:
    params = inspect.signature(uc2_cam_py.create_camera).parameters
    expected = {
        "vendor": None, "connection_type": None, "serial": None, "index": None,
        "buffer_size": None, "start_on_init": False, "image_file_path": None,
        "store_image_data": True, "logger": None, "debug_window": False, "separate_process": None,
    }
    for name, default in expected.items():
        assert name in params, name
        assert params[name].default == default, name


def test_debug_window_is_the_only_way_to_a_window() -> None:
    with pytest.raises(ModuleNotFoundError):
        importlib.import_module("uc2_cam_py.preview")
    assert "display" not in inspect.signature(uc2_cam_py.run_connection_test).parameters


def test_all_exports_exist() -> None:
    for name in uc2_cam_py.__all__:
        assert hasattr(uc2_cam_py, name), name
    assert set(uc2_cam_py.vendors()) >= {"HikRobot", "Simulated"}
