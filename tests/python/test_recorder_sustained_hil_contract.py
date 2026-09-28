from pathlib import Path
import shutil
import subprocess

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_recorder_sustained_hil.ps1"


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_sustained_hil_runner_is_passive_and_waits_for_real_cache_eviction():
    runner = read("tools/run_recorder_sustained_hil.ps1")

    assert '[string]$LogPort = "COM6"' in runner
    assert '"-Action", "monitor"' in runner
    assert '"-Action", "flash"' not in runner
    assert "This runner never flashes firmware" in runner
    assert "cacheCapacityFrames" in runner
    assert "requiredPersistedFrames" in runner
    assert "SavedReferenceFrames" in runner
    assert "EvictionMarginFrames" in runner
    assert "samples_written" in runner
    assert "/api/seismic/session" in runner
    assert "analyze_recorder_sustained.py" in runner
    assert "RECORDER_SUSTAINED_HIL_RESULT" in runner


def test_sustained_hil_runner_gates_every_recording_phase_on_transport_health():
    runner = read("tools/run_recorder_sustained_hil.ps1")

    assert "HealthSettleSeconds = 15" in runner
    assert '"{0}/api/status" -f $base' in runner
    assert "quality_window_frames" in runner
    assert "Get-ReportedSampleRateHz $Status" in runner
    assert "one second at" in runner
    assert "[ValidateRange(1, 40000)]" in runner
    assert 'streamState -ne "NORMAL" -and $streamState -ne "QUIET"' in runner
    assert 'Add-HealthSample $healthSamples "preflight"' in runner
    assert 'Add-HealthSample $healthSamples "during-start"' in runner
    assert 'Add-HealthSample $healthSamples "during-recording"' in runner
    assert '"during-stop"' in runner
    assert '"post"' in runner
    assert "$transportFaultObserved = $true" in runner
    assert '"--health-evidence", $healthJson' in runner
    assert "HEALTH_EVIDENCE" in runner


def test_integrity_contract_exercises_psram_to_internal_dma_to_sd_production_path():
    recorder = read("components/seismic_recorder/seismic_recorder.c")
    filesystem = read("components/seismic_recorder/seismic_recorder_fs.c")
    reader = read("components/seismic_recorder/seismic_recorder_reader.c")

    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in recorder
    assert "s_batch = heap_caps_malloc(" in recorder
    assert "s_cache_storage = heap_caps_malloc(" in recorder
    assert "PSRAM cache=%u frames" in recorder

    assert "DMA_ATTR static seismic_recorder_dma_staging_t s_dma_staging" in filesystem
    assert "memcpy(\n                s_dma_staging.v1_frames," in filesystem
    assert "fwrite(\n                s_dma_staging.v1_frames," in filesystem
    assert "fwrite(s_dma_staging.v2_window, 1U, encoded_bytes, samples_file)" in filesystem
    assert "RECORDER_IO_TIMING op=fwrite" in filesystem

    assert 'seismic_recorder_reader_log_path(\n                "cache"' in reader
    assert 'seismic_recorder_reader_log_path(\n                "raw"' in reader
    assert "WAVEFORM_READ_PATH source=%s" in reader


def test_sustained_analyzer_requires_throughput_integrity_and_shared_transport_health():
    analyzer = read("tools/analyze_recorder_sustained.py")
    health = read("tools/ad7606_health.py")

    assert "required_stream_bps" in analyzer
    assert "required_margin_bps" in analyzer
    assert "aggregate_write_bps" in analyzer
    assert "cache_capacity + frame_count" in analyzer
    assert 'match.group("source").lower() == "raw"' in analyzer
    assert "integrity mismatch" in analyzer
    assert "minimum_overlap_frames" in analyzer
    assert "classify_evidence" in analyzer
    assert "uart_transport_violations" in analyzer
    assert "--health-evidence" in analyzer
    assert "RECORDER_SUSTAINED_ANALYSIS" in analyzer

    assert "QUALITY_WINDOW_DURATION_SECONDS = 1" in health
    assert 'GOOD_STREAM_STATES = frozenset({"NORMAL", "QUIET"})' in health
    assert 'BAD_TRANSPORT_QUALITIES = frozenset({"DEGRADED", "FAULT"})' in health
    assert "signal_quality_states" in health


def test_sustained_hil_runner_parses_as_powershell_when_available():
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
