"""``uv run fetch_mvs``: extract the HikRobot MVS SDK from its Linux packages.

A package ``External/hik/MVS-<version>_Linux_<arch>_<date>.zip`` nests three archives:
the zip holds ``MVS-<version>_<arch>_<date>.tar.gz``, which holds ``MVS.tar.gz``, whose
tree is what an installation puts into /opt/MVS. Only the parts the build and the runtime
need go into ``External/hik/MVS``. The archives are third-party downloads, so every member
passes tarfile's "data" filter (no absolute paths, no "..", no device files).
"""

from __future__ import annotations

import re
import shutil
import tarfile
import zipfile
from pathlib import Path

from . import _root

#: Directories of the SDK tree taken over per package architecture.
ARCH_DIRS = {
    "x86_64": ("lib/64/", "lib/CLProtocol/Linux64_x64/"),
    "aarch64": ("lib/aarch64/", "lib/CLProtocol/Linux64_aarch64/"),
}
#: HikRobot's system setup scripts, next to MVS.tar.gz in the outer archive.
SCRIPTS = ("set_usb_priority.sh", "set_usbfs_memory_size.sh", "set_rp_filter.sh", "set_socket_buffer_size.sh")
PACKAGE = re.compile(r"MVS-(?P<version>[\d.]+)_Linux_(?P<arch>x86_64|aarch64)_(?P<date>\d+)\.zip")


def _wanted(name: str, arch: str) -> bool:
    return name.startswith(("include/", "license/", *ARCH_DIRS[arch]))


def _extract(package: Path, arch: str, dest: Path) -> str:
    """Extract one package into ``dest``; return the SDK version of its libMvCameraControl."""
    sdk_version = ""
    with zipfile.ZipFile(package) as zf:
        outer_name = next(n for n in zf.namelist() if n.endswith(".tar.gz"))
        with zf.open(outer_name) as raw, tarfile.open(fileobj=raw, mode="r|gz") as outer:
            for member in outer:
                base = member.name.rsplit("/", 1)[-1]
                if member.isfile() and base in SCRIPTS:
                    member.name = f"scripts/{base}"
                    outer.extract(member, dest, filter="data")
                elif member.isfile() and base == "MVS.tar.gz":
                    with tarfile.open(fileobj=outer.extractfile(member), mode="r|gz") as inner:
                        for m in inner:
                            name = m.name.removeprefix("./")
                            if not _wanted(name, arch):
                                continue
                            m.name = name
                            inner.extract(m, dest, filter="data")
                            found = re.fullmatch(r"lib/[^/]+/libMvCameraControl\.so\.([\d.]+)", name)
                            if found:
                                sdk_version = found.group(1)
    return sdk_version


def _newest_packages(hik: Path) -> dict[str, tuple[Path, re.Match[str]]]:
    best: dict[str, tuple[tuple, Path, re.Match[str]]] = {}
    for path in hik.glob("MVS-*_Linux_*.zip"):
        match = PACKAGE.fullmatch(path.name)
        if match is None:
            continue
        key = (tuple(int(p) for p in match["version"].split(".")), match["date"])
        if match["arch"] not in best or key > best[match["arch"]][0]:
            best[match["arch"]] = (key, path, match)
    return {arch: (path, match) for arch, (_, path, match) in best.items()}


def main() -> int:
    hik = _root() / "External" / "hik"
    packages = _newest_packages(hik)
    if not packages:
        print(f"fetch_mvs: no MVS Linux package (MVS-*_Linux_<arch>_*.zip) in {hik} - see its README.md")
        return 1
    versions = {match["version"] for _, match in packages.values()}
    if len(versions) > 1:
        print(f"fetch_mvs: the packages are of different versions ({', '.join(sorted(versions))}) - keep one")
        return 1

    staging = hik / "MVS.partial"
    shutil.rmtree(staging, ignore_errors=True)
    sdk: dict[str, str] = {}
    for arch, (path, _) in sorted(packages.items()):
        print(f"fetch_mvs: {path.name} -> External/hik/MVS ({arch})", flush=True)
        sdk[arch] = _extract(path, arch, staging)
        if not sdk[arch]:
            print(f"fetch_mvs: {path.name} holds no libMvCameraControl for {arch}")
            shutil.rmtree(staging, ignore_errors=True)
            return 1

    _, match = next(iter(packages.values()))
    (staging / "VERSION").write_text(
        f"package={match['version']}_{match['date']}\n"
        f"sdk={max(sdk.values())}\n"
        f"architectures={' '.join(sorted(sdk))}\n"
    )
    target = hik / "MVS"
    shutil.rmtree(target, ignore_errors=True)
    staging.rename(target)
    size = sum(f.stat().st_size for f in target.rglob("*") if f.is_file() and not f.is_symlink())
    print(f"fetch_mvs: MVS {match['version']} (SDK {', '.join(f'{a} {v}' for a, v in sorted(sdk.items()))}), "
          f"{size / 2**20:.0f} MiB in {target}")
    return 0
