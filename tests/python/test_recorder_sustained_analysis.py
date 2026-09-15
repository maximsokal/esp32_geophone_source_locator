from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "analyze_recorder_sustained.py"
SPEC = importlib.util.spec_from_file_location("analyze_recorder_sustained", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def health_status(
    *,
    quality: str = "HEALTHY",
    stream_state: str = "NORMAL",
    suspicious: bool = False,
    stream_stuck: bool = False,
    quality_window_frames: int = 500,
    consecutive_failures: int = 0,
    error_counters_available: bool = True,
    signal_quality: str = "HEALTHY",
) -> dict:
    return {
        "acquisition": {
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


def health_evidence(*samples: dict) -> dict:
    return {
        "samples": [
            {"phase": "preflight" if index == 0 else f"during-{index}", "status": sample}
            for index, sample in enumerate(samples)
        ]
    }


def make_evidence(
    *,
    operation_us: int = 10_000,
    source_frame_count: int = 300,
    read_source: str = "raw",
    mismatch: bool = False,
    write_samples: int = 30,
):
    session = "S000001"
    lines = [
        "I SEISMIC_RECORDER: Recording OBJ_0001/S000001 at 500 Hz from source cursor 100; PSRAM cache=100 frames, SD threshold=250 frames"
    ]
    for _ in range(write_samples):
        lines.append(
            "I SEISMIC_RECORDER_FS: RECORDER_IO_TIMING op=fwrite "
            "requested=256 written=256 windows=1 max_window_us=9000 "
            f"fwrite_us={operation_us} operation_us={operation_us} result=0"
        )
    lines.extend(
        [
            "I SEISMIC_RECORDER: STOP_COMPLETE stage=complete error_stage=none "
            "stage_error=ESP_OK session=S000001 samples=300 drops=0 err=ESP_OK",
            "I SEISMIC_RECORDER_READER: WAVEFORM_READ_PATH "
            f"source={read_source} session=S000001 start=0 frames=120 points=120 "
            "raw_frames=0 lod_records=0 lod_frames=0",
        ]
    )

    timestamps_us = [1_000_000 + index * 2_000 for index in range(120)]
    points = []
    for index, timestamp_us in enumerate(timestamps_us):
        channels = []
        for channel_id in range(1, 5):
            value = float(index * 10 + channel_id)
            channels.append(
                {"id": channel_id, "min_uv": value, "max_uv": value}
            )
        points.append({"timestamp_us": timestamp_us, "channels": channels})

    live_indices = range(20, 100)
    live = {
        "source": "AD7606",
        "sample_rate_hz": 500,
        "count": 80,
        "timestamps_ms": [timestamps_us[index] // 1000 for index in live_indices],
        "channels": [
            {
                "id": channel_id,
                "samples_uv": [
                    float(index * 10 + channel_id)
                    for index in live_indices
                ],
            }
            for channel_id in range(1, 5)
        ],
    }
    if mismatch:
        live["channels"][0]["samples_uv"][5] += 1.0

    saved = {
        "object_id": "OBJ_0001",
        "session_id": session,
        "source": "AD7606",
        "complete": True,
        "sample_rate_hz": 500,
        "channel_count": 4,
        "samples_written": 300,
        "dropped_frames": 0,
        "recording_ms": 70_000,
        "source_frame_count": source_frame_count,
        "start_frame": 0,
        "frame_count": 120,
        "count": 120,
        "points": points,
    }
    return "\n".join(lines) + "\n", live, saved


def analyze(log, live, saved, **overrides):
    options = {
        "sample_rate_hz": 500,
        "frame_size_bytes": 24,
        "safety_factor": 4.0,
        "minimum_write_samples": 30,
        "minimum_overlap_frames": 64,
        "integrity_tolerance_uv": 0.0015,
        "health_evidence": health_evidence(health_status()),
    }
    options.update(overrides)
    return MODULE.analyze(log, live, saved, **options)


def test_sustained_evidence_passes_with_margin_eviction_and_exact_sd_roundtrip():
    log, live, saved = make_evidence()

    result = analyze(log, live, saved)

    assert result.status == "PASS"
    assert result.cache_capacity_frames == 100
    assert result.write_samples == 30
    assert result.aggregate_write_bps is not None
    assert result.aggregate_write_bps > result.required_margin_bps
    assert result.integrity_overlap_frames == 80
    assert result.raw_sd_read_confirmed is True
    assert result.transport_health_status == "PASS"


def test_quiet_transport_with_clipped_signal_does_not_fail_storage_integrity():
    log, live, saved = make_evidence()

    result = analyze(
        log,
        live,
        saved,
        health_evidence=health_evidence(
            health_status(stream_state="QUIET", signal_quality="CLIPPED")
        ),
    )

    assert result.status == "PASS"
    assert result.transport_health_status == "PASS"
    assert result.signal_quality_states == ("CLIPPED",)


def test_immature_or_unknown_transport_health_blocks_sustained_pass():
    log, live, saved = make_evidence()

    result = analyze(
        log,
        live,
        saved,
        health_evidence=health_evidence(
            health_status(quality="UNKNOWN", quality_window_frames=100)
        ),
    )

    assert result.status == "BLOCKED"
    assert result.transport_health_status == "BLOCKED"
    assert any("immature" in item for item in result.missing)


def test_mature_transport_faults_fail_byte_perfect_roundtrip():
    log, live, saved = make_evidence()
    bad_samples = [
        health_status(quality="DEGRADED"),
        health_status(quality="FAULT"),
        health_status(stream_state="SUSPICIOUS"),
        health_status(stream_state="STUCK"),
        health_status(suspicious=True),
        health_status(stream_stuck=True),
        health_status(consecutive_failures=2),
    ]

    for sample in bad_samples:
        result = analyze(log, live, saved, health_evidence=health_evidence(sample))
        assert result.status == "FAIL"
        assert result.transport_health_status == "FAIL"


def test_in_run_transport_fault_remains_fail_after_recovery():
    log, live, saved = make_evidence()
    evidence = health_evidence(
        health_status(),
        health_status(quality="FAULT", stream_state="SUSPICIOUS", suspicious=True),
        health_status(),
    )

    result = analyze(log, live, saved, health_evidence=evidence)

    assert result.status == "FAIL"
    assert any("during-1" in item for item in result.violations)


def test_uart_mature_transport_fault_remains_fail_with_healthy_status_evidence():
    log, live, saved = make_evidence()
    log += (
        "I AD7606_DIAG: AD7606_SERIAL_DIAG: rx=00 00 class=ALL_00 "
        "transport_quality=FAULT signal_quality=HEALTHY stream_state=STUCK "
        "stream_stuck=yes busy=0 samples=500 streak=5 repeated_streak=500 all_zero=500 "
        "all_ff=0 identical=500 varied=0 digital_interference=0 signal_rail_hits=0 "
        "signal_clips=0 window=500 transport_bad=500 all_zero=500 all_ff=0 "
        "identical=500 repeated=500 digital=0 signal_rail=0 signal_clip=0 "
        "serial_suspicious=yes\n"
    )

    result = analyze(log, live, saved)

    assert result.status == "FAIL"
    assert result.uart_transport_fault_count == 1
    assert any("UART mature AD7606 transport fault" in item for item in result.violations)


def test_sustained_gate_rejects_insufficient_aggregate_write_margin():
    log, live, saved = make_evidence(operation_us=200_000)

    result = analyze(log, live, saved)

    assert result.status == "FAIL"
    assert any("aggregate recorder write throughput" in item for item in result.violations)


def test_integrity_range_must_be_evicted_from_psram_before_sd_claim():
    log, live, saved = make_evidence(source_frame_count=220)

    result = analyze(log, live, saved)

    assert result.status == "FAIL"
    assert any("not proven evicted from PSRAM cache" in item for item in result.violations)


def test_integrity_comparison_rejects_changed_channel_sample():
    log, live, saved = make_evidence(mismatch=True)

    result = analyze(log, live, saved)

    assert result.status == "FAIL"
    assert any("integrity mismatch" in item for item in result.violations)


def test_cache_fast_path_cannot_be_misreported_as_physical_sd_read():
    log, live, saved = make_evidence(read_source="cache")

    result = analyze(log, live, saved)

    assert result.status == "BLOCKED"
    assert result.raw_sd_read_confirmed is False
    assert any("source=raw" in item for item in result.missing)


def test_too_short_log_is_blocked_even_when_other_evidence_is_valid():
    log, live, saved = make_evidence(write_samples=5)

    result = analyze(log, live, saved)

    assert result.status == "BLOCKED"
    assert any("fwrite samples (5/30)" in item for item in result.missing)
