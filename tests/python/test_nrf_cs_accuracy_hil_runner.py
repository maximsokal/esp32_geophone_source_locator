"""Replay and contract coverage for the two-board Channel Sounding accuracy gate."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_nrf_cs_accuracy_hil.ps1"
HIL_APP = REPO_ROOT / "tests" / "target" / "nrf_cs_accuracy_hil"


def powershell_command() -> str:
    executable = shutil.which("powershell.exe") or shutil.which("pwsh")
    if executable is None:
        raise RuntimeError("PowerShell is required for Channel Sounding HIL runner tests")
    return executable


def make_log(distances: tuple[int, ...]) -> str:
    lines = [
        "NRF_CS_ACCURACY_HIL_READY version=1 repeats=20",
        "NRF_CS_ACCURACY_HIL_TOPOLOGY main=0 remote=1 assigned=2",
    ]
    for cycle_id, distance in enumerate(distances, 1):
        sample = {
            "cycle_id": cycle_id,
            "pair_index": 0,
            "pair_count": 1,
            "node_a": 0,
            "node_b": 1,
            "state": 2,
            "last_failure": 0,
            "distance_mm": distance,
            "confidence": 900,
            "provider_quality_flags": 0,
            "scheduler_quality_flags": 0,
            "attempts": 1,
            "valid_samples": 3,
            "error_code": 0,
        }
        lines.append(f"NRF_CS_ACCURACY_HIL_SAMPLE {json.dumps(sample, separators=(',', ':'))}")
    lines.append("NRF_CS_ACCURACY_HIL_END")
    return "\n".join(lines) + "\n"


def run_replay(path: Path, output_dir: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    args = [
        powershell_command(),
        "-NoLogo",
        "-NoProfile",
    ]
    if powershell_command().lower().endswith("powershell.exe"):
        args.extend(["-ExecutionPolicy", "Bypass"])
    args.extend(
        [
            "-File",
            str(RUNNER),
            "-LogPath",
            str(path),
            "-OutputDir",
            str(output_dir),
            "-ExpectedDistanceMm",
            "1000",
            "-MaxErrorMm",
            "20",
            "-Repeats",
            str(len(path.read_text(encoding="utf-8").splitlines()) - 3),
            "-MinSuccessRate",
            "1.0",
            "-FixtureConfirmed",
            *extra,
        ]
    )
    return subprocess.run(
        args,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def test_replay_passes_only_with_complete_quality_approved_samples(tmp_path: Path):
    path = tmp_path / "cs-pass.log"
    path.write_text(make_log((1000, 1002, 998, 1005, 995)), encoding="utf-8")

    result = run_replay(path, tmp_path / "pass-artifacts")

    assert result.returncode == 0
    assert "NRF_CS_ACCURACY_HIL_RESULT: PASS" in result.stdout
    summary = json.loads((tmp_path / "pass-artifacts" / "summary.json").read_text(encoding="utf-8"))
    assert summary["sample_count"] == 5
    assert summary["accepted_sample_count"] == 5
    assert summary["p95_abs_error_mm"] == 5
    assert summary["topology_ready"] is True


def test_replay_fails_on_outlier_even_when_the_cycle_count_is_complete(tmp_path: Path):
    path = tmp_path / "cs-outlier.log"
    path.write_text(make_log((1000, 1002, 1040, 1005, 995)), encoding="utf-8")

    result = run_replay(path, tmp_path / "fail-artifacts")

    assert result.returncode == 1
    assert "NRF_CS_ACCURACY_HIL_RESULT: FAIL" in result.stdout
    assert "maximum error" in result.stdout


def test_runner_blocks_without_fixture_confirmation_or_two_node_marker(tmp_path: Path):
    path = tmp_path / "cs-no-fixture.log"
    path.write_text(make_log((1000, 1002, 998, 1005, 995)).replace(" assigned=2", " assigned=1"), encoding="utf-8")

    result = run_replay(path, tmp_path / "blocked-artifacts")

    assert result.returncode == 2
    assert "NRF_CS_ACCURACY_HIL_RESULT: BLOCKED" in result.stdout


def test_hil_runner_and_app_are_explicit_non_destructive_two_board_contracts():
    runner = RUNNER.read_text(encoding="utf-8")
    app = (HIL_APP / "main" / "nrf_cs_accuracy_hil.c").read_text(encoding="utf-8")

    for marker in (
        "NRF_CS_ACCURACY_HIL_SAMPLE",
        "NRF_CS_ACCURACY_HIL_RESULT",
        "ExpectedDistanceMm",
        "MinSuccessRate",
        "MinConfidence",
        "FixtureConfirmed",
        "nrf_topology_service_request_cs_cycle",
        "nrf_topology_service_get_cs_cycle",
    ):
        assert marker in runner or marker in app

    assert "erase_flash" not in runner.lower()
    assert "nvs_erase" not in runner.lower()
    assert "espefuse" not in runner.lower()
    assert "NRF_CS_ACCURACY_HIL_REPEATS 20U" in app
    assert "assigned_remotes == 1U" in app
