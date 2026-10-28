"""Export recordings (``.uc2rb``) as OME-TIFF.

The camera records into ``.uc2rb`` files, a format only this package reads. This
module turns them into OME-TIFF, which Fiji/Bio-Formats, napari, tifffile and
other tools open directly::

    from uc2_cam_py import exporter

    for path in exporter.list_raw_files():          # the default recording folder
        with exporter.export_as_ome_tiff(path) as tif:
            print(tif.filename, tif.series[0].shape)   # (T, H, W) or (T, H, W, 3)

What goes where in the OME-TIFF:

- **Session header** (the header of the whole file): in the OME-XML of page 0 as a
  ``MapAnnotation`` of the image (namespace :data:`SESSION_NAMESPACE`), and as
  JSON in the private TIFF tag :data:`SESSION_HEADER_TAG` of page 0.
- **Frame header** (the header of every image): in the OME-XML as the ``Plane``
  of the image (``DeltaT`` = time since the session start) with a
  ``MapAnnotation`` (namespace :data:`FRAME_NAMESPACE`), and as JSON in the
  private TIFF tag :data:`FRAME_HEADER_TAG` of the image's own page.

``TheT`` counts the images in the file; the ``sequence`` in the frame header is
the camera's frame number, so frames lost while recording show as gaps there.
"""

from __future__ import annotations

import dataclasses
import json
import os
import uuid
import xml.etree.ElementTree as ET
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import _hik, _uc2rb, defaults
from .data import ImgMetaData

if TYPE_CHECKING:
    import tifffile

__all__ = ["list_raw_files", "export_as_ome_tiff"]

#: Private TIFF tag on every page: that image's frame header (JSON).
FRAME_HEADER_TAG = 65400
#: Private TIFF tag on page 0: the session header of the recording (JSON).
SESSION_HEADER_TAG = 65401
#: Namespace of the session header's MapAnnotation in the OME-XML.
SESSION_NAMESPACE = "openuc2.org/uc2_cam_py/session"
#: Namespace of the frame headers' MapAnnotations in the OME-XML.
FRAME_NAMESPACE = "openuc2.org/uc2_cam_py/frame"

_OME = "http://www.openmicroscopy.org/Schemas/OME/2016-06"
_OME_TYPES = {"uint8": "uint8", "uint16": "uint16", "float64": "double"}
_BIGTIFF_ABOVE = 2**32 - 2**25   # bytes; classic TIFF offsets end at 4 GiB


def list_raw_files(folder: str | os.PathLike[str] | None = None) -> list[str]:
    """The recordings (``*.uc2rb``) in a folder.

    Args:
        folder: Folder to look in (not its subfolders). None: the default recording
            folder, ``defaults.IMAGE_FILE_PATH``.

    Returns:
        list[str]: Absolute paths, sorted by name - recordings are named
        ``uc2_<camera>_<UTC time>.uc2rb``, so this is the order they were made in.
        Empty if the default folder does not exist yet.

    Raises:
        TypeError: ``folder`` is not a str, a path or None.
        FileNotFoundError: The given folder does not exist.
        NotADirectoryError: ``folder`` is a file.
    """
    if folder is None:
        root = Path(defaults.IMAGE_FILE_PATH)
        if not root.exists():
            return []
    elif isinstance(folder, (str, os.PathLike)):
        root = Path(folder)
        if not root.exists():
            raise FileNotFoundError(f"folder {str(root)!r} does not exist")
    else:
        raise TypeError(f"folder must be a str, a path or None, got {type(folder).__name__}")
    if not root.is_dir():
        raise NotADirectoryError(f"{str(root)!r} is not a folder")
    return [str(p.absolute()) for p in sorted(root.glob("*.uc2rb")) if p.is_file()]


def export_as_ome_tiff(
    path: str | os.PathLike[str], *, output_path: str | os.PathLike[str] | None = None
) -> tifffile.TiffFile:
    """Export a recording as OME-TIFF and open the result.

    Writes ``<name>.ome.tif`` next to the recording (or to ``output_path``),
    replacing an existing file. The images are copied one by one, so the
    recording may be larger than the memory.

    Args:
        path: The ``.uc2rb`` recording.
        output_path: Where to write the OME-TIFF instead.

    Returns:
        tifffile.TiffFile: The written file, **open** - close it, best with
        ``with export_as_ome_tiff(path) as tif: ...``. ``tif.series[0]`` holds the
        images, ``(T, H, W)`` for one channel or ``(T, H, W, 3)`` for RGB/BGR.

    Raises:
        TypeError: ``path`` or ``output_path`` is not a str or a path.
        FileNotFoundError: The recording does not exist.
        ValueError: Not a ``.uc2rb`` recording, an unknown layout, or no images in it.
    """
    import tifffile  # here, not at the top: importing uc2_cam_py stays fast

    for name, value in (("path", path), ("output_path", output_path)):
        if value is not None and not isinstance(value, (str, os.PathLike)):
            raise TypeError(f"{name} must be a str or a path, got {type(value).__name__}")
    source = Path(path)
    if not source.is_file():
        raise FileNotFoundError(f"recording {str(source)!r} does not exist")
    target = Path(output_path) if output_path is not None else source.with_name(source.stem + ".ome.tif")

    with source.open("rb") as f:
        meta = _uc2rb.read_session_header(f, source)
        records = _uc2rb.frame_records(f, source, meta)
        if not records:
            raise ValueError(f"{source} holds no images")
        xml = _ome_xml(meta, records, source)
        session_json = json.dumps(_session_fields(meta, source))
        rgb = meta.channels > 1
        bigtiff = len(records) * meta.frame_bytes + len(xml) > _BIGTIFF_ABOVE

        # Into a temporary file first: an interrupted export leaves no half-written OME-TIFF.
        part = target.with_name(target.name + ".part")
        try:
            with tifffile.TiffWriter(part, bigtiff=bigtiff, byteorder="<", ome=False) as tw:
                for t, rec in enumerate(records):
                    frame = _uc2rb.read_frame(f, rec, meta)
                    tags = [(FRAME_HEADER_TAG, "s", 0, json.dumps(rec.header()), False)]
                    if t == 0:
                        tags.append((SESSION_HEADER_TAG, "s", 0, session_json, False))
                    # One write per image: tifffile puts the extratags of a contiguous
                    # series on its first page only, but every page carries its own header.
                    tw.write(
                        frame if rgb else frame[..., 0],
                        photometric="rgb" if rgb else "minisblack",
                        description=xml if t == 0 else None,
                        metadata=None,
                        software=f"uc2_cam_py {_hik.__version__}",
                        extratags=tags,
                    )
            part.replace(target)
        except BaseException:
            part.unlink(missing_ok=True)
            raise
    return tifffile.TiffFile(target)


# ---------------------------------------------------------------------------
# OME-XML
# ---------------------------------------------------------------------------

def _session_fields(meta: ImgMetaData, source: Path) -> dict[str, Any]:
    """Every field of the session header, plus where the export came from."""
    fields: dict[str, Any] = dataclasses.asdict(meta)
    fields["source_file"] = source.name
    fields["exporter"] = f"uc2_cam_py {_hik.__version__}"
    return fields


def _physical_size_um(meta: ImgMetaData) -> tuple[float, float] | None:
    """Pixel size in the image (x, y) in um: pixel pitch x binning, if the comment has both."""
    try:
        info = json.loads(meta.comment)
        pitch = info["pixel_pitch_um"]
        binning = int(info.get("binning") or 1)
        return float(pitch[0]) * binning, float(pitch[1]) * binning
    except (ValueError, KeyError, TypeError, IndexError):
        return None


def _q(tag: str) -> str:
    return f"{{{_OME}}}{tag}"


def _map_annotation(parent: ET.Element, ann_id: str, namespace: str, fields: dict[str, Any]) -> None:
    ann = ET.SubElement(parent, _q("MapAnnotation"), ID=ann_id, Namespace=namespace)
    value = ET.SubElement(ann, _q("Value"))
    for key, val in fields.items():
        ET.SubElement(value, _q("M"), K=key).text = str(val)


def _ome_xml(meta: ImgMetaData, records: list[_uc2rb.FrameRecord], source: Path) -> str:
    """The OME-XML for page 0: one image, one plane per frame, headers as annotations."""
    ET.register_namespace("", _OME)
    root = ET.Element(_q("OME"), UUID=f"urn:uuid:{uuid.uuid4()}", Creator=f"uc2_cam_py {_hik.__version__}")

    # Child order as the OME schema demands: Image(AcquisitionDate, Pixels, AnnotationRef),
    # Pixels(Channel, TiffData, Plane), then StructuredAnnotations after the image.
    image = ET.SubElement(root, _q("Image"), ID="Image:0", Name=source.stem)
    if meta.start_wall_ns > 0:
        start = datetime.fromtimestamp(meta.start_wall_ns / 1e9, tz=UTC)
        ET.SubElement(image, _q("AcquisitionDate")).text = start.strftime("%Y-%m-%dT%H:%M:%S.%fZ")

    pixels_attrs = {
        "ID": "Pixels:0",
        "DimensionOrder": "XYCZT",
        "Type": _OME_TYPES.get(meta.dtype, "uint8"),
        "SizeX": str(meta.width),
        "SizeY": str(meta.height),
        "SizeZ": "1",
        "SizeC": str(meta.channels),
        "SizeT": str(len(records)),
        "Interleaved": "true" if meta.channels > 1 else "false",
        "BigEndian": "false",
    }
    size = _physical_size_um(meta)
    if size is not None:   # the unit defaults to um
        pixels_attrs["PhysicalSizeX"] = repr(size[0])
        pixels_attrs["PhysicalSizeY"] = repr(size[1])
    pixels = ET.SubElement(image, _q("Pixels"), pixels_attrs)
    ET.SubElement(pixels, _q("Channel"), ID="Channel:0:0", SamplesPerPixel=str(meta.channels))
    ET.SubElement(pixels, _q("TiffData"), IFD="0", PlaneCount=str(len(records)))

    t0 = meta.start_mono_ns if meta.start_mono_ns > 0 else records[0].time_ns
    for t, rec in enumerate(records):
        plane = ET.SubElement(pixels, _q("Plane"), TheZ="0", TheC="0", TheT=str(t),
                              DeltaT=f"{(rec.time_ns - t0) / 1e9:.9f}", DeltaTUnit="s")
        ET.SubElement(plane, _q("AnnotationRef"), ID=f"Annotation:Frame:{t}")
    ET.SubElement(image, _q("AnnotationRef"), ID="Annotation:Session")

    annotations = ET.SubElement(root, _q("StructuredAnnotations"))
    _map_annotation(annotations, "Annotation:Session", SESSION_NAMESPACE, _session_fields(meta, source))
    for t, rec in enumerate(records):
        _map_annotation(annotations, f"Annotation:Frame:{t}", FRAME_NAMESPACE, {
            "sequence": rec.sequence,
            "time_before_ns": rec.time_before_ns,
            "time_after_ns": rec.time_after_ns,
            "time_ns": rec.time_ns,
            "slot": rec.slot,
            "flags": rec.flags,
        })

    # ASCII only (other characters as references): TIFF's ImageDescription is ASCII.
    body = ET.tostring(root, encoding="unicode").encode("ascii", "xmlcharrefreplace").decode("ascii")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + body
