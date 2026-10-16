#!/usr/bin/env python3
"""Recreate selected third_party files from pinned sibling Git checkouts.

Does not download, delete old files, or modify the upstream checkouts.
Run in a clean destination when changing the source selection.
"""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

project = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else project.parent
dest = project / "third_party"
metadata = json.loads((source / "SOURCES.json").read_text(encoding="utf-8-sig"))
selected = ("wfb-ng", "msposd", "rtl88x2eu-20230815", "research", "firmware")
pins = {entry["directory"]: entry for entry in metadata["repositories"]}
for name in selected:
    actual = subprocess.check_output(["git", "-C", str(source / name), "rev-parse", "HEAD"], text=True).strip()
    if actual != pins[name]["commit"]:
        raise SystemExit(f"Commit mismatch for {name}: {actual}")
    if subprocess.check_output(["git", "-C", str(source / name), "status", "--porcelain", "--untracked-files=no"], text=True):
        raise SystemExit(f"Tracked modifications in {name}; refusing an unpinned snapshot")

def copy_file(src, relative):
    output = dest / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, output)

for name in selected[:3]:
    files = subprocess.check_output(["git", "-C", str(source / name), "ls-files", "-z"]).decode().split("\0")
    for item in filter(None, files):
        path = pathlib.PurePosixPath(item)
        if ".git" in path.parts or path.suffix in (".o", ".ko", ".a", ".pyc"):
            continue
        copy_file(source / name / path, pathlib.Path(name) / path)

copies = {
    "research/star/include/infinity6e": "sdk/sigmastar/include",
    "research/sdk/hi3536dv100/include": "sdk/hisilicon/include",
    "firmware/general/package/sigmastar-osdrv-infinity6e/files/lib": "sdk/sigmastar/lib",
    "firmware/general/package/hisilicon-osdrv-hi3536dv100/files/lib": "sdk/hisilicon/lib",
}
for original, target in copies.items():
    for path in sorted((source / original).rglob("*")):
        if path.is_file() and ".git" not in path.parts:
            copy_file(path, pathlib.Path(target) / path.relative_to(source / original))
copy_file(source / "research/LICENSE", "sdk/RESEARCH-LICENSE")
copy_file(source / "firmware/LICENSE", "sdk/FIRMWARE-LICENSE")
for package in ("sigmastar-osdrv-infinity6e", "hisilicon-osdrv-hi3536dv100"):
    base = source / "firmware/general/package" / package
    copy_file(base / f"{package}.mk", pathlib.Path("sdk") / f"{package}.mk")
    for path in base.rglob("LICENSE*"):
        if path.is_file():
            copy_file(path, pathlib.Path("sdk") / f"{package}-{path.name}")
metadata["copied_components"] = list(selected[:3]) + [f"{a} -> {b}" for a, b in copies.items()]
(dest / "SOURCES.json").write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
hashes = []
for path in sorted(dest.rglob("*")):
    if path.is_file() and path.name != "SHA256SUMS":
        hashes.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(dest).as_posix()}")
(dest / "SHA256SUMS").write_text("\n".join(hashes) + "\n", encoding="utf-8")
print(f"Vendored and hashed {len(hashes)} files from checked commits")
