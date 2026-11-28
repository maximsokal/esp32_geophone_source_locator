"""Repository hygiene checks: forbid tracked CMake/compiler/test build artifacts."""
from __future__ import annotations

import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def test_no_tracked_cmake_or_object_build_artifacts() -> None:
    proc = subprocess.run(
        ["git", "ls-files"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    tracked = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    forbidden: list[str] = []
    for path in tracked:
        name = Path(path).name
        parts = Path(path).parts
        if name == "CMakeCache.txt" or "CMakeFiles" in parts or "build_host" in parts:
            forbidden.append(path)
        elif name.endswith((".o", ".obj")) or (
            path.startswith("PocketBeagle2/build/") and not name.endswith(".md")
        ):
            forbidden.append(path)
    assert not forbidden, f"Tracked build artifacts found in git index: {forbidden[:10]}"
