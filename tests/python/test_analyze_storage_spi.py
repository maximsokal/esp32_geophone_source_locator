from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = REPO_ROOT / "tools" / "analyze_storage_spi.py"

_spec = importlib.util.spec_from_file_location("analyze_storage_spi", ANALYZER_PATH)
assert _spec is not None and _spec.loader is not None
analyzer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = analyzer
_spec.loader.exec_module(analyzer)


LIMITS = dict(
    max_mutex_wait_us=100,
    max_tft_quiesce_us=200,
    max_tft_resume_us=150,
    max_spi_end_elapsed_us=1000,
    max_recorder_window_us=500,
    max_recorder_fwrite_us=700,
    max_recorder_operation_us=1200,
)


def healthy_log() -> str:
    lines = [
        "I STORAGE: STORAGE_SPI_TIMING stage=begin mutex_wait_us=80 tft_quiesce_us=180 result=0",
        "I STORAGE: STORAGE_SPI_TIMING stage=end tft_resume_us=120 elapsed_us=900",
        "I REC: RECORDER_IO_TIMING op=fwrite requested=4096 written=4096 windows=2 max_window_us=450 fwrite_us=650 operation_us=1100 result=0",
        "I REC: STOP_COMPLETE stage=complete session=S0001 frames=1000 drops=0 err=ESP_OK",
    ]
    for channel in range(1, 9):
        lines.append(f"I AD7606: AD7606_DIAG: CH{channel} n=100 mean=0")
    return "\n".join(lines) + "\n"


def test_healthy_log_passes():
    result = analyzer.analyze(healthy_log(), **LIMITS)

    assert result.status == "PASS"
    assert result.missing == ()
    assert result.violations == ()
    assert result.max_mutex_wait_us == 80
    assert result.max_tft_quiesce_us == 180
    assert result.max_tft_resume_us == 120
    assert result.max_spi_end_elapsed_us == 900
    assert result.max_recorder_window_us == 450
    assert result.max_recorder_fwrite_us == 650
    assert result.max_recorder_operation_us == 1100
    assert result.max_dropped_frames == 0
    assert result.acquisition_diagnostic_channels == tuple(range(1, 9))


def test_exact_threshold_boundaries_pass():
    content = healthy_log()
    replacements = {
        "mutex_wait_us=80": "mutex_wait_us=100",
        "tft_quiesce_us=180": "tft_quiesce_us=200",
        "tft_resume_us=120": "tft_resume_us=150",
        "elapsed_us=900": "elapsed_us=1000",
        "max_window_us=450": "max_window_us=500",
        "fwrite_us=650": "fwrite_us=700",
        "operation_us=1100": "operation_us=1200",
    }
    for old, new in replacements.items():
        content = content.replace(old, new)

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "PASS"
    assert result.violations == ()


def test_over_limit_values_fail():
    content = healthy_log().replace("mutex_wait_us=80", "mutex_wait_us=101")
    content = content.replace("max_window_us=450", "max_window_us=501")

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "FAIL"
    assert "mutex_wait_us=101us exceeds limit=100us" in result.violations
    assert "max_window_us=501us exceeds limit=500us" in result.violations


def test_missing_telemetry_is_blocked():
    content = "\n".join(
        line
        for line in healthy_log().splitlines()
        if "STORAGE_SPI_TIMING stage=end" not in line
        and "RECORDER_IO_TIMING" not in line
        and "STOP_COMPLETE" not in line
    )

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "BLOCKED"
    assert any("STORAGE_SPI_TIMING stage=end" in item for item in result.missing)
    assert any("RECORDER_IO_TIMING op=fwrite" in item for item in result.missing)
    assert any("SEISMIC_RECORDER STOP_COMPLETE" in item for item in result.missing)


def test_short_write_zero_windows_and_result_fail():
    content = healthy_log().replace(
        "requested=4096 written=4096 windows=2",
        "requested=4096 written=2048 windows=0",
    ).replace("operation_us=1100 result=0", "operation_us=1100 result=-1")

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "FAIL"
    assert "recorder fwrite result=-1" in result.violations
    assert "recorder short write requested=4096 written=2048" in result.violations
    assert "recorder fwrite reported zero storage windows" in result.violations


def test_storage_begin_result_fails():
    content = healthy_log().replace("tft_quiesce_us=180 result=0", "tft_quiesce_us=180 result=-2")

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "FAIL"
    assert "storage begin result=-2" in result.violations


def test_stop_drop_stage_and_error_fail():
    content = healthy_log().replace(
        "STOP_COMPLETE stage=complete session=S0001 frames=1000 drops=0 err=ESP_OK",
        "STOP_COMPLETE stage=flush session=S0001 frames=1000 drops=3 err=ESP_FAIL",
    )

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "FAIL"
    assert "recorder stop stage=flush expected=complete" in result.violations
    assert "recorder dropped_frames=3 expected=0" in result.violations
    assert "recorder stop err=ESP_FAIL expected=ESP_OK" in result.violations
    assert result.max_dropped_frames == 3


def test_acquisition_failure_and_panic_fail():
    content = healthy_log() + "E AD7606_ACQ: Read failed err=ESP_FAIL\nE CPU: Guru Meditation Error: Core 1 panic'ed\n"

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "FAIL"
    assert result.acquisition_failure_count == 1
    assert result.failure_marker_count == 1


def test_diagnostic_channel_requirement_blocks_incomplete_capture():
    content = "\n".join(
        line
        for line in healthy_log().splitlines()
        if "AD7606_DIAG: CH8" not in line
    )

    result = analyzer.analyze(content, **LIMITS)

    assert result.status == "BLOCKED"
    assert result.acquisition_diagnostic_channels == tuple(range(1, 8))
    assert any("(7/8)" in item for item in result.missing)
