"""Deterministic checks for the host-test manifest itself."""

import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tests" / "host" / "manifest.json"


def test_host_manifest_lists_existing_sources_and_unique_names():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = manifest["tests"]
    names = [entry["name"] for entry in entries]

    assert len(names) == len(set(names))
    assert all(
        (REPO_ROOT / source).is_file()
        for entry in entries
        for source in entry["sources"]
    )
