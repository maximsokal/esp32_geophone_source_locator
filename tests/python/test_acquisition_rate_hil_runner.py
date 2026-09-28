"""Contracts for the user-run AD7606 acquisition-rate HIL capture."""

from pathlib import Path
import shutil
import subprocess

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_acquisition_rate_hil.ps1"


def test_acquisition_rate_runner_collects_required_status_and_log_evidence():
    runner = RUNNER.read_text(encoding="utf-8")

    for rate in ("5000", "10000", "20000", "40000"):
        assert rate in runner
    for field in (
        "timer_ticks",
        "conversion_attempts",
        "collapsed_periods",
        "read_errors",
        "samples_written",
        "dropped_frames",
        "RECORDER_IO_TIMING",
        "RUNTIME_CPU",
        "poll task STACK_HWM",
    ):
        assert field in runner
    assert "MaxRecorderOperationUs" in runner
    assert "ACQUISITION_RATE_HIL_RESULT" in runner
    assert '"-Action", "flash"' not in runner


def test_acquisition_rate_runner_parses_as_powershell_when_available():
    executable = shutil.which("powershell.exe") or shutil.which("pwsh")
    if executable is None:
        pytest.skip("PowerShell is unavailable on this host")

    command = (
        "$ErrorActionPreference='Stop'; "
        f"$text = Get-Content -LiteralPath '{RUNNER}' -Raw; "
        "[void][scriptblock]::Create($text)"
    )
    result = subprocess.run(
        [executable, "-NoLogo", "-NoProfile", "-Command", command],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )

    assert result.returncode == 0, result.stderr or result.stdout


def test_storage_hil_document_covers_rates_jumpers_preamp_and_scope():
    documentation = (REPO_ROOT / "docs" / "storage-spi-latency-hil.md").read_text(encoding="utf-8")

    for text in (
        "run_acquisition_rate_hil.ps1",
        "5 kHz, 10 kHz, 20 kHz, or 40 kHz",
        "OS2 OS1 OS0",
        "Use OS×4 at 20 kSPS",
        "OS×8 cannot run at 40 kSPS",
        "AD620 + LM358 preamp sweep",
        "span_ms=2000&max_points=1200",
        "Scope checks",
        "64 clocks",
    ):
        assert text in documentation
