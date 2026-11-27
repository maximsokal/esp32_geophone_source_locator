#!/usr/bin/env python3
"""Reproducible downloader and verifier of the public 3D-scanning test datasets (Phase 2 benchmarks).

Datasets land OUTSIDE the repository (default E:\\datasets\\3d-scanning); only this script and `3d-scanning.manifest.json` (URLs, sizes, SHA-256,
licences, camera facts) are tracked. Nothing here is a phone capture: TUM RGB-D (Kinect), ICL-NUIM and Redwood augmented ICL-NUIM (synthetic) are used as
independent depth/pose replay and ground-truth geometry for precision/recall and performance, never as a claim about ARCore depth.

  python tools/datasets/download_3d_scanning.py download [--root DIR] [--only NAME ...]   resumable download, SHA-256, extraction
  python tools/datasets/download_3d_scanning.py verify   [--root DIR]                    re-check sizes/hashes, depth<->pose timestamp sync, frame counts
  python tools/datasets/download_3d_scanning.py list

A SHA-256 of an item that is not yet in the manifest is recorded on the first successful download (trust on first use over HTTPS: the sites publish no
checksums); later runs fail on any difference.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "3d-scanning.manifest.json"
DEFAULT_ROOT = Path(r"E:\datasets\3d-scanning")
USER_AGENT = "geolocator-dataset-fetch/1.0"


def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def save_manifest(manifest: dict) -> None:
    MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def remote_size(url: str) -> int | None:
    request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            value = response.headers.get("Content-Length")
            return int(value) if value else None
    except Exception:
        return None


def fetch(url: str, destination: Path) -> None:
    """Resumable download (HTTP Range) into destination + '.part'."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    part = destination.with_name(destination.name + ".part")
    total = remote_size(url)
    for attempt in range(1, 7):
        have = part.stat().st_size if part.exists() else 0
        if total is not None and have == total:
            break
        headers = {"User-Agent": USER_AGENT}
        if have:
            headers["Range"] = f"bytes={have}-"
        request = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                mode = "ab" if have and response.status == 206 else "wb"
                with part.open(mode) as handle:
                    last = time.time()
                    while True:
                        block = response.read(1 << 20)
                        if not block:
                            break
                        handle.write(block)
                        if time.time() - last > 30:
                            print(f"    {destination.name}: {handle.tell() / 1e6:.0f} MB", flush=True)
                            last = time.time()
            if total is None or part.stat().st_size == total:
                break
        except Exception as error:  # noqa: BLE001 - retried below, reported at the end
            print(f"    attempt {attempt} failed: {error}", flush=True)
            time.sleep(min(60, 5 * attempt))
    size = part.stat().st_size if part.exists() else 0
    if total is not None and size != total:
        raise RuntimeError(f"incomplete download of {url}: {size} of {total} bytes")
    part.replace(destination)


def extract(archive: Path, target: Path) -> None:
    marker = target / ".extracted"
    if marker.exists():
        return
    target.mkdir(parents=True, exist_ok=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(target)
    elif archive.name.endswith((".tgz", ".tar.gz")):
        with tarfile.open(archive) as bundle:
            bundle.extractall(target)
    else:
        return
    marker.write_text(time.strftime("%Y-%m-%dT%H:%M:%S") + "\n", encoding="utf-8")


def cmd_list(manifest: dict) -> None:
    for item in manifest["items"]:
        print(f"{item['name']:34} {item['dataset']:10} {item.get('bytes', 0) / 1e6:8.0f} MB  {item['license']}")


def cmd_download(manifest: dict, root: Path, only: list[str]) -> int:
    root.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    for item in manifest["items"]:
        if only and item["name"] not in only:
            continue
        destination = root / item["path"]
        print(f"[{item['name']}] {item['url']}", flush=True)
        try:
            if not destination.exists():
                fetch(item["url"], destination)
            digest = sha256_of(destination)
            size = destination.stat().st_size
            if item.get("sha256") and item["sha256"] != digest:
                raise RuntimeError(f"SHA-256 differs from the manifest: {digest}")
            item["sha256"] = digest
            item["bytes"] = size
            if item.get("extract"):
                extract(destination, root / item["extract"])
            save_manifest(manifest)
            print(f"    ok {size / 1e6:.0f} MB sha256={digest[:16]}…", flush=True)
        except Exception as error:  # noqa: BLE001 - one unavailable source must not stop the others
            failures.append(f"{item['name']}: {error}")
            print(f"    FAILED: {error}", flush=True)
    if failures:
        print("\nUnavailable or failed items (the rest are complete):")
        for line in failures:
            print("  -", line)
        return 1
    return 0


def associate(first: list[tuple[float, str]], second: list[tuple[float, list[float]]], max_dt: float = 0.02) -> tuple[int, float, float]:
    """TUM-style nearest-timestamp association: returns (pairs, mean dt, max dt) in seconds."""
    pairs, deltas = 0, []
    index = 0
    for stamp, _ in first:
        while index + 1 < len(second) and abs(second[index + 1][0] - stamp) <= abs(second[index][0] - stamp):
            index += 1
        delta = abs(second[index][0] - stamp)
        if delta <= max_dt:
            pairs += 1
            deltas.append(delta)
    return pairs, (sum(deltas) / len(deltas) if deltas else 0.0), (max(deltas) if deltas else 0.0)


def read_list(path: Path) -> list[tuple[float, str]]:
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line and not line.startswith("#"):
            parts = line.split()
            rows.append((float(parts[0]), parts[1] if len(parts) > 1 else ""))
    return rows


def read_poses(path: Path) -> list[tuple[float, list[float]]]:
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line and not line.startswith("#"):
            parts = line.split()
            rows.append((float(parts[0]), [float(value) for value in parts[1:]]))
    return rows


def cmd_verify(manifest: dict, root: Path) -> int:
    bad = 0
    for item in manifest["items"]:
        destination = root / item["path"]
        if not destination.exists():
            print(f"[{item['name']}] MISSING")
            bad += 1
            continue
        if item.get("sha256") and sha256_of(destination) != item["sha256"]:
            print(f"[{item['name']}] SHA-256 MISMATCH")
            bad += 1
            continue
        print(f"[{item['name']}] ok ({destination.stat().st_size / 1e6:.0f} MB)")
    for item in manifest["items"]:
        if item.get("kind") != "tum-sequence" or not (root / item["extract"]).exists():
            continue
        directory = next((p for p in (root / item["extract"]).iterdir() if p.is_dir()), None)
        if directory is None:
            continue
        depth = read_list(directory / "depth.txt")
        truth = read_poses(directory / "groundtruth.txt")
        pairs, mean_dt, max_dt = associate(depth, truth)
        frames = len(list((directory / "depth").glob("*.png")))
        status = "ok" if pairs >= 0.95 * len(depth) and frames == len(depth) else "CHECK"
        print(f"[{item['name']}] depth frames {frames}/{len(depth)}, depth<->pose pairs {pairs} (mean dt {mean_dt * 1000:.1f} ms, max {max_dt * 1000:.1f} ms) {status}")
        bad += status != "ok"
    return 1 if bad else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["download", "verify", "list"])
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--only", nargs="*", default=[])
    arguments = parser.parse_args()
    manifest = load_manifest()
    if arguments.command == "list":
        cmd_list(manifest)
        return 0
    if arguments.command == "download":
        return cmd_download(manifest, arguments.root, arguments.only)
    return cmd_verify(manifest, arguments.root)


if __name__ == "__main__":
    sys.exit(main())
