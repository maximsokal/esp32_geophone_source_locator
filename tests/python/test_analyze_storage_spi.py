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


def status_sample(
    *,
    quality: str = "HEALTHY",
    stream_state: str = "NORMAL",
    suspicious: bool = False,
    stream_stuck: bool = False,
    sample_rate_hz: int = 500,
    quality_window_frames: int = 500,
    consecutive_failures: int = 0,
    error_counters_available: bool = True,
    signal_quality: str = "HEALTHY",
) -> dict:
    return {
        "acquisition": {
            "sample_rate_hz": sample_rate_hz,
            "error_counters_available": error_counters_available,
            "consecutive_failures": consecutive_failures,
            "serial": {
                "quality": quality,
                "stream_state": stream_state,
                "suspicious": suspicious,
                "stream_stuck": stream_stuck,
                "quality_window_frames": quality_window_frames,
            },
            "signal": {"quality": signal_quality},
        }
    }


def evidence(*samples: dict) -> dict:
    return {
        "samples": [
            {"phase": "preflight" if index == 0 else f"sample-{index}", "status": sample}
            for index, sample in enumerate(samples)
        ]
    }


HEALTHY_EVIDENCE = evidence(status_sample())

LIMITS = dict(
    max_mutex_wait_us=100,
    max_tft_quiesce_us=200,
    max_tft_resume_us=150,
    max_spi_end_elapsed_us=1000,
    max_recorder_window_us=500,
    max_recorder_fwrite_us=700,
    max_recorder_operation_us=1200,
    health_evidence=HEALTHY_EVIDENCE,
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


def analyze(content: str, health_evidence: dict = HEALTHY_EVIDENCE):
    options = dict(LIMITS)
    options["health_evidence"] = health_evidence
    return analyzer.analyze(content, **options)


def test_healthy_log_passes():
    result = analyze(healthy_log())

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
    assert result.transport_health_status == "PASS"


def test_healthy_quiet_transport_and_clipped_signal_still_pass_storage_gate():
    result = analyze(
        healthy_log(),
        evidence(status_sample(stream_state="QUIET", signal_quality="CLIPPED")),
    )

    assert result.status == "PASS"
    assert result.transport_health_status == "PASS"
    assert result.signal_quality_states == ("CLIPPED",)


def test_immature_or_unknown_transport_health_blocks_pass():
    immature = analyze(
        healthy_log(),
        evidence(status_sample(quality="UNKNOWN", quality_window_frames=499)),
    )
    unavailable = analyze(healthy_log(), None)

    assert immature.status == "BLOCKED"
    assert immature.transport_health_status == "BLOCKED"
    assert any("immature" in item for item in immature.missing)
    assert unavailable.status == "BLOCKED"
    assert any("transport health" in item for item in unavailable.missing)


def test_health_window_maturity_tracks_reported_one_second_sample_rate():
    immature = analyze(
        healthy_log(),
        evidence(status_sample(sample_rate_hz=20_000, quality_window_frames=19_999)),
    )
    mature = analyze(
        healthy_log(),
        evidence(status_sample(sample_rate_hz=20_000, quality_window_frames=20_000)),
    )

    assert immature.transport_health_status == "BLOCKED"
    assert any("19999/20000" in item for item in immature.missing)
    assert mature.transport_health_status == "PASS"


def test_mature_transport_faults_fail_even_when_storage_bytes_are_clean():
    cases = [
        status_sample(quality="DEGRADED"),
        status_sample(quality="FAULT"),
        status_sample(stream_state="SUSPICIOUS"),
        status_sample(stream_state="STUCK"),
        status_sample(suspicious=True),
        status_sample(stream_stuck=True),
        status_sample(consecutive_failures=1),
    ]

    for sample in cases:
        result = analyze(healthy_log(), evidence(sample))
        assert result.status == "FAIL"
        assert result.transport_health_status == "FAIL"


def test_recovered_final_health_does_not_hide_in_run_fault():
    health = evidence(
        status_sample(),
        status_sample(quality="FAULT", stream_state="SUSPICIOUS", suspicious=True),
        status_sample(),
    )

    result = analyze(healthy_log(), health)

    assert result.status == "FAIL"
    assert any("sample-1" in item for item in result.violations)


def test_mature_uart_transport_fault_is_sticky_even_with_healthy_status_evidence():
    serial_diag = (
        "I AD7606_DIAG: AD7606_SERIAL_DIAG: rx=00 00 class=ALL_00 "
        "transport_quality=FAULT signal_quality=HEALTHY stream_state=SUSPICIOUS "
        "stream_stuck=no busy=0 samples=500 streak=5 repeated_streak=0 all_zero=500 "
        "all_ff=0 identical=0 varied=0 digital_interference=0 signal_rail_hits=0 "
        "signal_clips=0 window=500 transport_bad=500 all_zero=500 all_ff=0 "
        "identical=0 repeated=0 digital=0 signal_rail=0 signal_clip=0 "
        "serial_suspicious=yes\n"
    )

    result = analyze(healthy_log() + serial_diag)

    assert result.status == "FAIL"
    assert result.uart_transport_fault_count == 1
    assert any("UART mature AD7606 transport fault" in item for item in result.violations)


def test_uart_window_uses_reported_sample_rate():
    serial_diag = (
        "I AD7606_DIAG: AD7606_SERIAL_DIAG: transport_quality=FAULT "
        "signal_quality=HEALTHY stream_state=STUCK stream_stuck=yes "
        "window=500 serial_suspicious=yes\n"
    )
    result = analyze(
        healthy_log() + serial_diag,
        evidence(status_sample(sample_rate_hz=20_000, quality_window_frames=20_000)),
    )
    assert result.status == "PASS"
    assert result.uart_transport_fault_count == 0


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

    result = analyze(content)

    assert result.status == "PASS"
    assert result.violations == ()


def test_over_limit_values_fail():
    content = healthy_log().replace("mutex_wait_us=80", "mutex_wait_us=101")
    content = content.replace("max_window_us=450", "max_window_us=501")

    result = analyze(content)

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

    result = analyze(content)

    assert result.status == "BLOCKED"
    assert any("STORAGE_SPI_TIMING stage=end" in item for item in result.missing)
    assert any("RECORDER_IO_TIMING op=fwrite" in item for item in result.missing)
    assert any("SEISMIC_RECORDER STOP_COMPLETE" in item for item in result.missing)


def test_short_write_zero_windows_and_result_fail():
    content = healthy_log().replace(
        "requested=4096 written=4096 windows=2",
        "requested=4096 written=2048 windows=0",
    ).replace("operation_us=1100 result=0", "operation_us=1100 result=-1")

    result = analyze(content)

    assert result.status == "FAIL"
    assert "recorder fwrite result=-1" in result.violations
    assert "recorder short write requested=4096 written=2048" in result.violations
    assert "recorder fwrite reported zero storage windows" in result.violations


def test_storage_begin_result_fails():
    content = healthy_log().replace("tft_quiesce_us=180 result=0", "tft_quiesce_us=180 result=-2")

    result = analyze(content)

    assert result.status == "FAIL"
    assert "storage begin result=-2" in result.violations


def test_stop_drop_stage_and_error_fail():
    content = healthy_log().replace(
        "STOP_COMPLETE stage=complete session=S0001 frames=1000 drops=0 err=ESP_OK",
        "STOP_COMPLETE stage=flush session=S0001 frames=1000 drops=3 err=ESP_FAIL",
    )

    result = analyze(content)

    assert result.status == "FAIL"
    assert "recorder stop stage=flush expected=complete" in result.violations
    assert "recorder dropped_frames=3 expected=0" in result.violations
    assert "recorder stop err=ESP_FAIL expected=ESP_OK" in result.violations
    assert result.max_dropped_frames == 3


def test_acquisition_failure_and_panic_fail():
    content = healthy_log() + "E AD7606_ACQ: Read failed err=ESP_FAIL\nE CPU: Guru Meditation Error: Core 1 panic'ed\n"

    result = analyze(content)

    assert result.status == "FAIL"
    assert result.acquisition_failure_count == 1
    assert result.failure_marker_count == 1


def test_diagnostic_channel_requirement_blocks_incomplete_capture():
    content = "\n".join(
        line
        for line in healthy_log().splitlines()
        if "AD7606_DIAG: CH8" not in line
    )

    result = analyze(content)

    assert result.status == "BLOCKED"
    assert result.acquisition_diagnostic_channels == tuple(range(1, 8))
    assert any("(7/8)" in item for item in result.missing)
