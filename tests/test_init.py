"""create_camera(): argument validation, device selection and init logging."""

from __future__ import annotations

import pytest

import uc2_cam_py
from uc2_cam_py import defaults

from conftest import SIM_OPTIONS, hik_camera_connected, wait_for_frames


def test_first_camera_is_opened_without_serial_and_index(open_camera) -> None:
    cam = open_camera()
    assert cam.vendor == "Simulated"
    assert cam.index == 0
    assert cam.serial == "SIM-0001"


def test_serial_and_index_select_the_camera(open_camera) -> None:
    assert open_camera(serial="SIM-4711").serial == "SIM-4711"
    assert open_camera(index=0).index == 0
    with pytest.raises(IndexError):
        open_camera(index=1)


@pytest.mark.parametrize(
    "kwargs, error",
    [
        (dict(serial="A", index=0), ValueError),
        (dict(vendor="Basler"), ValueError),
        (dict(vendor=3), TypeError),
        (dict(connection_type="Firewire"), ValueError),
        (dict(index=-1), ValueError),
        (dict(index=1.0), TypeError),
        (dict(serial=""), ValueError),
        (dict(buffer_size=1), ValueError),
        (dict(buffer_size="8"), TypeError),
        (dict(start_on_init=1), TypeError),
        (dict(store_image_data="yes"), TypeError),
        (dict(debug_window="yes"), TypeError),
        (dict(separate_process=1), TypeError),
        (dict(sensor_width="400"), TypeError),   # vendor options are type-checked for both paths
        (dict(frame_rate=True), TypeError),
        (dict(pixel_format="Mono12"), ValueError),
        (dict(image_file_path=42), TypeError),
        (dict(logger=object()), TypeError),
        (dict(colour="red"), TypeError),       # unknown vendor option
    ],
)
def test_invalid_arguments_raise(kwargs, error, tmp_path) -> None:
    args = dict(vendor="Simulated", store_image_data=False, image_file_path=tmp_path, **SIM_OPTIONS)
    args.update(kwargs)
    with pytest.raises(error):
        uc2_cam_py.create_camera(**args)


def test_connection_type_is_case_insensitive_and_mirrored(open_camera) -> None:
    assert open_camera(connection_type="gige").connection_type == "GigE"
    assert open_camera(connection_type="USB3").connection_type == "USB3"


def test_start_on_init_starts_recording(open_camera) -> None:
    cam = open_camera(start_on_init=True)
    assert cam.is_recording
    wait_for_frames(cam, 2)


def test_init_information_is_logged(open_camera, rec_logger) -> None:
    open_camera()
    infos = "\n".join(rec_logger.messages("INFO"))
    assert "Camera initialised: Simulated UC2-SIM, serial SIM-0001" in infos
    assert "Sensor 400x200 px" in infos
    assert "Buffer 8 slots" in infos
    assert "Settable:" in infos
    assert "SimCamera: simulated 400x200 sensor" in infos   # forwarded from C++


def test_default_vendor_comes_from_defaults(monkeypatch, tmp_path, rec_logger) -> None:
    monkeypatch.setattr(defaults, "VENDOR", "Simulated")
    with uc2_cam_py.create_camera(store_image_data=False, logger=rec_logger, **SIM_OPTIONS) as cam:
        assert cam.vendor == "Simulated"


def test_buffer_size_default(monkeypatch, open_camera) -> None:
    monkeypatch.setattr(defaults, "BUFFER_SIZE", 5)
    assert open_camera(buffer_size=None).buffer_size == 5


def test_context_manager_closes(open_camera) -> None:
    cam = open_camera(start_on_init=True)
    with cam as c:
        assert c is cam
    assert not cam.is_recording
    with pytest.raises(RuntimeError):
        cam.start()


def test_missing_hikrobot_camera_raises_and_logs(rec_logger, tmp_path) -> None:
    if hik_camera_connected():
        pytest.skip("a camera is connected")
    with pytest.raises(RuntimeError, match="no HikRobot camera"):
        uc2_cam_py.create_camera(vendor="HikRobot", store_image_data=False, logger=rec_logger)
    assert rec_logger.has("ERROR", "Opening a HikRobot camera failed")


@pytest.mark.hardware
def test_hikrobot_by_serial_and_index(open_camera) -> None:
    if not hik_camera_connected():
        pytest.skip("no HikRobot camera connected")
    first = uc2_cam_py.list_devices()[0]
    cam = open_camera("HikRobot", serial=first.serial)
    assert cam.serial == first.serial
    assert cam.connection_type == first.transport
    cam.close()
    assert open_camera("HikRobot", index=0).serial == first.serial


# ------------------------------------------------------------------ list_cameras

def test_list_cameras_offers_one_simulated_camera_that_opens(open_camera) -> None:
    cameras = uc2_cam_py.list_cameras()
    assert {c.vendor for c in cameras} <= set(uc2_cam_py.vendors())
    sims = [c for c in cameras if c.vendor == "Simulated"]
    assert sims == [uc2_cam_py.CameraInfo("Simulated", "UC2-SIM", "SIM-0001", "")]
    cam = open_camera(sims[0].vendor, serial=sims[0].serial)
    assert (cam.model, cam.serial) == (sims[0].model, sims[0].serial)


def test_list_cameras_skips_a_vendor_whose_search_fails(monkeypatch, caplog) -> None:
    def fail(cls):
        raise RuntimeError("SDK not initialised")

    monkeypatch.setattr(uc2_cam_py.HikRobotCamera, "_discover", classmethod(fail))
    assert [c.vendor for c in uc2_cam_py.list_cameras()] == ["Simulated"]
    assert "Searching HikRobot cameras failed: SDK not initialised" in caplog.text


@pytest.mark.hardware
def test_list_cameras_lists_every_hikrobot_camera() -> None:
    if not hik_camera_connected():
        pytest.skip("no HikRobot camera connected")
    hik = [c for c in uc2_cam_py.list_cameras() if c.vendor == "HikRobot"]
    assert [c.serial for c in hik] == [d.serial for d in uc2_cam_py.list_devices()]
    assert all(c.connection_type in uc2_cam_py.CONNECTION_TYPES for c in hik)
