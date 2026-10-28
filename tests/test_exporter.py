"""uc2_cam_py.exporter: list recordings, export them as OME-TIFF (and the .uc2rb reader behind it)."""

from __future__ import annotations

import itertools
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
import pytest
import tifffile

import uc2_cam_py
from uc2_cam_py import _uc2rb, defaults, exporter

from conftest import wait_for_frames

OME = {"ome": "http://www.openmicroscopy.org/Schemas/OME/2016-06"}
ROOT = Path(__file__).resolve().parents[1]


# ------------------------------------------------------------------ helpers

def _record(open_camera, frames: int = 8, **kwargs) -> tuple[str, uc2_cam_py.ImgMetaData]:
    """A real recording of the simulated camera (400 x 200, 100 fps): its path and session header."""
    cam = open_camera(store_image_data=True, **kwargs)
    cam.start()
    wait_for_frames(cam, frames)
    cam.stop()                                  # completes the file
    return cam.recording_file, cam.get_image_info()


def _write_uc2rb(path: Path, frames: np.ndarray, sequences: list[int] | None = None, header_bytes: int = 8448) -> None:
    """A synthetic recording of ``frames`` (T, H, W, C), written with the reader's own layout."""
    t_count, height, width, channels = frames.shape
    frame_bytes = height * width * channels * frames.itemsize
    sequences = list(range(t_count)) if sequences is None else sequences
    h = np.zeros((), _uc2rb.SESSION_HEADER)
    h["magic"] = b"UC2RB"
    h["formatVersion"] = 1
    h["headerBytes"] = header_bytes
    h["width"], h["height"], h["channels"], h["depth"] = width, height, channels, frames.itemsize
    h["frameBytes"] = frame_bytes
    h["slotCount"], h["layoutVersion"] = 4, 2
    h["startWallNs"], h["startMonoNs"] = 1_700_000_000_000_000_000, 5_000_000_000
    h["slotStride"] = frame_bytes
    h["cameraId"], h["softwareVersion"] = b"TEST-0001", b"9.9.9"
    h["comment"] = json.dumps({"pixel_pitch_um": [3.45, 3.45], "binning": 2}).encode()
    with path.open("wb") as f:
        f.write(h.tobytes())
        for seq, frame in zip(sequences, frames, strict=True):
            fh = np.zeros((), _uc2rb.FRAME_HEADER)
            fh["sequence"], fh["nbytes"] = seq, frame.nbytes
            fh["tsBeforeNs"] = 5_000_000_000 + seq * 10_000_000
            fh["tsAfterNs"] = fh["tsBeforeNs"] + 2_000
            fh["slot"], fh["flags"] = seq % 4, 0b1010
            fh["width"], fh["height"], fh["channels"], fh["depth"] = width, height, channels, frames.itemsize
            f.write(fh.tobytes())
            f.write(np.ascontiguousarray(frame).tobytes())


def _read_back(path: str | Path) -> tuple[uc2_cam_py.ImgMetaData, list[_uc2rb.FrameRecord], list[np.ndarray]]:
    with open(path, "rb") as f:
        meta = _uc2rb.read_session_header(f, path)
        records = _uc2rb.frame_records(f, path, meta)
        return meta, records, [_uc2rb.read_frame(f, r, meta) for r in records]


def _annotation(root: ET.Element, ann_id: str) -> dict[str, str]:
    ann = root.find(f".//ome:MapAnnotation[@ID='{ann_id}']", OME)
    assert ann is not None, ann_id
    return {m.get("K"): m.text or "" for m in ann.iterfind("ome:Value/ome:M", OME)}


def _page_json(tif: tifffile.TiffFile, index: int, tag: int) -> dict:
    return json.loads(tif.pages.get(index, aspage=True).tags[tag].value)


# ------------------------------------------------------------------ list_raw_files

def test_list_raw_files_lists_the_recordings_of_a_folder(tmp_path) -> None:
    for name in ("b.uc2rb", "a.uc2rb", "a.json", "notes.txt"):
        (tmp_path / name).write_bytes(b"")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "c.uc2rb").write_bytes(b"")        # not recursive
    (tmp_path / "d.uc2rb").mkdir()                          # a folder, not a recording
    expected = [str(tmp_path / "a.uc2rb"), str(tmp_path / "b.uc2rb")]
    assert exporter.list_raw_files(str(tmp_path)) == expected
    assert exporter.list_raw_files(tmp_path) == expected    # a Path works too
    assert all(Path(p).is_absolute() for p in expected)


def test_list_raw_files_uses_the_default_folder(tmp_path, monkeypatch) -> None:
    monkeypatch.setattr(defaults, "IMAGE_FILE_PATH", tmp_path / "recordings")
    assert exporter.list_raw_files() == []                  # no recording yet, no folder yet
    (tmp_path / "recordings").mkdir()
    (tmp_path / "recordings" / "x.uc2rb").write_bytes(b"")
    assert exporter.list_raw_files(None) == [str(tmp_path / "recordings" / "x.uc2rb")]


def test_list_raw_files_rejects_bad_folders(tmp_path) -> None:
    with pytest.raises(FileNotFoundError):
        exporter.list_raw_files(tmp_path / "missing")
    (tmp_path / "file.uc2rb").write_bytes(b"")
    with pytest.raises(NotADirectoryError):
        exporter.list_raw_files(tmp_path / "file.uc2rb")
    with pytest.raises(TypeError):
        exporter.list_raw_files(42)                          # type: ignore[arg-type]


# ------------------------------------------------------------------ the reader

def test_reader_matches_the_header_the_camera_wrote(open_camera) -> None:
    """The numpy layout of the reader equals the C++ structs: same header as the camera reports."""
    assert _uc2rb.SESSION_HEADER.itemsize == 8448 and _uc2rb.FRAME_HEADER.itemsize == 64
    path, meta = _record(open_camera)
    read, records, frames = _read_back(path)
    assert read == meta                                      # every field, from the file
    assert len(records) >= 8
    assert all(b.sequence > a.sequence for a, b in itertools.pairwise(records))
    assert all(r.time_before_ns <= r.time_after_ns for r in records)
    assert frames[0].shape == (meta.height, meta.width, 1) and frames[0].dtype == np.uint16


# ------------------------------------------------------------------ export of real recordings

@pytest.mark.parametrize("pixel_format, dtype", [("Mono16", np.uint16), ("Mono8", np.uint8)])
def test_a_recording_exports_as_ome_tiff(open_camera, pixel_format, dtype) -> None:
    path, meta = _record(open_camera, pixel_format=pixel_format)
    _, records, frames = _read_back(path)
    tif = exporter.export_as_ome_tiff(path)
    try:
        assert not tif.filehandle.closed                     # returned open
        assert tif.filename == Path(path).stem + ".ome.tif"
        assert Path(tif.filehandle.path).parent == Path(path).parent
        assert tif.is_ome
        series = tif.series[0]
        assert series.shape == (len(records), meta.height, meta.width)
        assert series.dtype == dtype
        assert np.array_equal(series.asarray(), np.stack(frames)[..., 0])

        # Session header: OME-XML annotation and tag on page 0.
        root = ET.fromstring(tif.ome_metadata)
        session = _annotation(root, "Annotation:Session")
        assert session["camera_id"] == meta.camera_id
        assert session["software_version"] == uc2_cam_py.__version__
        assert session["comment"] == meta.comment
        assert session["start_mono_ns"] == str(meta.start_mono_ns)
        assert session["source_file"] == Path(path).name
        tag = _page_json(tif, 0, exporter.SESSION_HEADER_TAG)
        assert {k: tag[k] for k in ("camera_id", "width", "start_wall_ns")} == {
            "camera_id": meta.camera_id, "width": meta.width, "start_wall_ns": meta.start_wall_ns}
        assert root.find("ome:Image/ome:AcquisitionDate", OME) is not None
        pixels = root.find("ome:Image/ome:Pixels", OME)
        assert pixels.get("SizeT") == str(len(records)) and pixels.get("SizeC") == "1"
        assert float(pixels.get("PhysicalSizeX")) == pytest.approx(5.86)   # simulated pitch x binning 1

        # Frame headers: a Plane with its annotation, and a tag on the image's own page.
        planes = root.findall("ome:Image/ome:Pixels/ome:Plane", OME)
        assert [int(p.get("TheT")) for p in planes] == list(range(len(records)))
        delta = [float(p.get("DeltaT")) for p in planes]
        assert all(b > a for a, b in itertools.pairwise(delta))
        for t, (plane, rec) in enumerate(zip(planes, records, strict=True)):
            ann_id = plane.find("ome:AnnotationRef", OME).get("ID")
            assert int(_annotation(root, ann_id)["sequence"]) == rec.sequence
            page = _page_json(tif, t, exporter.FRAME_HEADER_TAG)
            assert page == rec.header()
    finally:
        tif.close()


def test_export_to_another_path_replaces_an_existing_file(open_camera, tmp_path) -> None:
    path, _ = _record(open_camera, frames=3)
    target = tmp_path / "out" / "export.ome.tif"
    target.parent.mkdir()
    target.write_bytes(b"old")
    with exporter.export_as_ome_tiff(path, output_path=target) as tif:
        assert Path(tif.filehandle.path) == target and tif.is_ome
    assert not list(target.parent.glob("*.part"))
    assert not Path(path).with_name(Path(path).stem + ".ome.tif").exists()   # nothing next to the source


# ------------------------------------------------------------------ synthetic recordings

@pytest.mark.parametrize("dtype, ome_type", [(np.uint8, "uint8"), (np.uint16, "uint16"), (np.float64, "double")])
def test_one_channel_of_any_depth(tmp_path, dtype, ome_type) -> None:
    frames = (np.arange(3 * 6 * 5).reshape(3, 6, 5, 1) * 7).astype(dtype)
    _write_uc2rb(tmp_path / "r.uc2rb", frames)
    with exporter.export_as_ome_tiff(tmp_path / "r.uc2rb") as tif:
        assert np.array_equal(tif.series[0].asarray(), frames[..., 0])
        root = ET.fromstring(tif.ome_metadata)
        pixels = root.find("ome:Image/ome:Pixels", OME)
        assert pixels.get("Type") == ome_type
        assert float(pixels.get("PhysicalSizeX")) == pytest.approx(3.45 * 2)   # pitch x binning


def test_rgb_recordings_keep_their_three_samples(tmp_path) -> None:
    frames = np.random.default_rng(1).integers(0, 255, (3, 6, 5, 3), dtype=np.uint8)
    _write_uc2rb(tmp_path / "rgb.uc2rb", frames)
    with exporter.export_as_ome_tiff(tmp_path / "rgb.uc2rb") as tif:
        assert tif.series[0].shape == (3, 6, 5, 3)
        assert tif.pages[0].photometric == tifffile.PHOTOMETRIC.RGB
        assert np.array_equal(tif.series[0].asarray(), frames)
        pixels = ET.fromstring(tif.ome_metadata).find("ome:Image/ome:Pixels", OME)
        assert (pixels.get("SizeC"), pixels.get("Interleaved")) == ("3", "true")
        assert pixels.find("ome:Channel", OME).get("SamplesPerPixel") == "3"


def test_lost_frames_show_as_gaps_in_the_sequence(tmp_path) -> None:
    frames = np.zeros((3, 4, 4, 1), np.uint16)
    _write_uc2rb(tmp_path / "gap.uc2rb", frames, sequences=[5, 6, 9])
    with exporter.export_as_ome_tiff(tmp_path / "gap.uc2rb") as tif:
        root = ET.fromstring(tif.ome_metadata)
        planes = root.findall("ome:Image/ome:Pixels/ome:Plane", OME)
        assert [p.get("TheT") for p in planes] == ["0", "1", "2"]
        seqs = [int(_annotation(root, p.find("ome:AnnotationRef", OME).get("ID"))["sequence"]) for p in planes]
        assert seqs == [5, 6, 9]
        assert [_page_json(tif, t, exporter.FRAME_HEADER_TAG)["sequence"] for t in range(3)] == [5, 6, 9]


def test_a_cut_off_last_frame_is_left_out_with_a_warning(tmp_path, caplog) -> None:
    frames = np.ones((4, 8, 8, 1), np.uint16)
    path = tmp_path / "cut.uc2rb"
    _write_uc2rb(path, frames)
    path.write_bytes(path.read_bytes()[:-10])               # the recording was interrupted
    with exporter.export_as_ome_tiff(path) as tif:
        assert tif.series[0].shape == (3, 8, 8)
    assert "frame 3 is cut off" in caplog.text


def test_bad_input_is_refused(tmp_path) -> None:
    with pytest.raises(FileNotFoundError):
        exporter.export_as_ome_tiff(tmp_path / "missing.uc2rb")
    with pytest.raises(TypeError):
        exporter.export_as_ome_tiff(42)                      # type: ignore[arg-type]
    (tmp_path / "junk.uc2rb").write_bytes(b"not a recording" * 100)
    with pytest.raises(ValueError, match="not a uc2rb recording"):
        exporter.export_as_ome_tiff(tmp_path / "junk.uc2rb")
    _write_uc2rb(tmp_path / "empty.uc2rb", np.zeros((0, 4, 4, 1), np.uint8))
    with pytest.raises(ValueError, match="no images"):
        exporter.export_as_ome_tiff(tmp_path / "empty.uc2rb")
    _write_uc2rb(tmp_path / "old.uc2rb", np.zeros((1, 4, 4, 1), np.uint8), header_bytes=512)
    with pytest.raises(ValueError, match="512 bytes"):
        exporter.export_as_ome_tiff(tmp_path / "old.uc2rb")
    assert not list(tmp_path.glob("*.ome.tif*"))              # nothing half-written left behind


# ------------------------------------------------------------------ the example program

def test_the_example_program_runs(tmp_path) -> None:
    """Examples/export_recordings.py --demo: records with the simulated camera, exports, reads back."""
    out = subprocess.run(
        [sys.executable, str(ROOT / "Examples" / "export_recordings.py"), "--demo", str(tmp_path)],
        capture_output=True, text=True, timeout=120, check=False,
    )
    assert out.returncode == 0, out.stderr
    assert "-> " in out.stdout and ".ome.tif" in out.stdout
    assert "sequence" in out.stdout
    assert list(tmp_path.glob("*.ome.tif"))
