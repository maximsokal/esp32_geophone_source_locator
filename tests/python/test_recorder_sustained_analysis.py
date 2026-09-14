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
