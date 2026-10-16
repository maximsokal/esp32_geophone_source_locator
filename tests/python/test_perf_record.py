"""Strict performance-record gates and the deterministic virtual-board run."""

from __future__ import annotations

from contextlib import contextmanager
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

import pytest

from tools.validate_perf_record import (
    NOT_APPLICABLE,
    PerfRecordError,
    attach_build_evidence,
    compare_records,
    migrate_legacy_record,
    prepare_baseline_update,
    record_errors,
    validate_record,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = REPO_ROOT / ".test-build"
HOST_RUNNER = REPO_ROOT / "tools" / "run_host_tests.py"
PERF_PREFIX = "PERF_RECORD_JSON: "


def valid_record() -> dict[str, object]:
    return {
        "schema_version": 2,
        "firmware_sha256": NOT_APPLICABLE,
        "build_profile": NOT_APPLICABLE,
        "optimization_mode": NOT_APPLICABLE,
        "test_surface": "HOST_VIRTUAL",
        "sample_rate_hz": 500,
        "channel_count": 4,
        "sample_format": "float32_uv",
        "channel_sentinel_order": ["frame", "negative_frame", "frame_plus_quarter", "frame_minus_quarter"],
        "sd_card_identity": NOT_APPLICABLE,
        "sd_card_frequency_hz": NOT_APPLICABLE,
        "workload_id": "healthy",
        "seed": 2226001714,
        "duration_ms": 3500,
        "classification": "PASS",
        "produced_frames": 100,
        "ingested_frames": 100,
        "persisted_frames": 100,
        "explicitly_dropped_frames": 0,
        "explicitly_rejected_frames": 0,
        "resident_frames": 0,
        "acquisition_service_jitter_us": NOT_APPLICABLE,
        "recorder_write_latency_us": {"p50": 120, "p95": 160, "p99": 180, "max": 200},
        "bytes_per_write": 2400,
        "writes_per_second": 2,
        "ring_high_water_frames": 5,
        "cache_high_water_frames": 100,
        "queue_high_water": 1,
        "cpu_runtime_us": NOT_APPLICABLE,
        "task_runtime_us": NOT_APPLICABLE,
        "core_runtime_us": NOT_APPLICABLE,
        "internal_heap_free_bytes": NOT_APPLICABLE,
        "internal_heap_largest_block_bytes": NOT_APPLICABLE,
        "psram_free_bytes": NOT_APPLICABLE,
        "psram_largest_block_bytes": NOT_APPLICABLE,
        "stack_hwm_bytes": NOT_APPLICABLE,
        "web_stop_latency_ms": NOT_APPLICABLE,
        "web_status_latency_ms": NOT_APPLICABLE,
        "live_latency_ms": NOT_APPLICABLE,
        "reset_markers": {"unexpected_reset": NOT_APPLICABLE, "watchdog": NOT_APPLICABLE, "brownout": NOT_APPLICABLE},
        "project_hot_path_allocation_violations": NOT_APPLICABLE,
        "active_allocation_count": NOT_APPLICABLE,
        "diagnostic_reports_suppressed": NOT_APPLICABLE,
        "diagnostic_reports_coalesced": NOT_APPLICABLE,
        "v2_quantizer_calls": NOT_APPLICABLE,
        "raw_path_selection": NOT_APPLICABLE,
        "seismic_buffer_atomic_operations": NOT_APPLICABLE,
        "seismic_buffer_critical_section_entries": NOT_APPLICABLE,
        "seismic_buffer_retries": NOT_APPLICABLE,
        "ad7606_status_critical_section_entries": NOT_APPLICABLE,
        "ad7606_status_critical_section_total_us": NOT_APPLICABLE,
        "ad7606_status_critical_section_max_us": NOT_APPLICABLE,
        "frame_duplication_detected": False,
        "frame_reordering_detected": False,
        "channel_swap_detected": False,
        "corrupted_finalized_file": NOT_APPLICABLE,
        "corrupted_finalized_session": NOT_APPLICABLE,
        "dirty_cache_overwrite_detected": False,
        "diagnostic_integer_overflow_detected": NOT_APPLICABLE,
        "deadlock_detected": False,
        "workload_supported": True,
        "unsupported_workload_disposition": NOT_APPLICABLE,
    }


@contextmanager
def host_compile_lock(timeout_seconds: float = 900.0):
    """Serialize native builds and always release only a lock this test acquired."""

    lock = BUILD_ROOT / "host.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            lock.mkdir()
            break
        except FileExistsError:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out waiting for host compile lock: {lock}")
            time.sleep(5)
    try:
        yield
    finally:
        lock.rmdir()


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("frame_duplication_detected", True),
        ("frame_reordering_detected", True),
        ("channel_swap_detected", True),
        ("corrupted_finalized_file", True),
        ("corrupted_finalized_session", True),
        ("dirty_cache_overwrite_detected", True),
        ("diagnostic_integer_overflow_detected", True),
        ("deadlock_detected", True),
        ("project_hot_path_allocation_violations", 1),
        ("active_allocation_count", 1),
    ],
)
def test_hard_failure_markers_are_rejected(field, value):
    record = valid_record()
    record[field] = value
    assert record_errors(record)


@pytest.mark.parametrize("marker", ["unexpected_reset", "watchdog", "brownout"])
def test_reset_watchdog_and_brownout_markers_are_rejected(marker):
    record = valid_record()
    record["reset_markers"][marker] = True
    with pytest.raises(PerfRecordError, match="reset marker"):
        validate_record(record)


def test_unaccounted_frame_loss_is_rejected():
    record = valid_record()
    record["persisted_frames"] -= 1
    with pytest.raises(PerfRecordError, match="unaccounted frame loss"):
        validate_record(record)


def test_v1_stack_hwm_migration_preserves_the_byte_value():
    legacy = valid_record()
    legacy.update(
        test_surface="TARGET_STRESS",
        firmware_sha256="d" * 64,
        build_profile="PERFORMANCE",
        optimization_mode="performance",
        sd_card_identity="measured-card",
        sd_card_frequency_hz=20_000_000,
    )
    legacy["schema_version"] = 1
    legacy["stack_hwm_words"] = 1536
    legacy.pop("stack_hwm_bytes")

    migrated = migrate_legacy_record(legacy)

    assert migrated["schema_version"] == 2
    assert migrated["stack_hwm_bytes"] == 1536
    assert "stack_hwm_words" not in migrated
    validate_record(legacy)


def test_swapped_channel_sentinels_are_rejected():
    record = valid_record()
    record["channel_sentinel_order"][1], record["channel_sentinel_order"][2] = (
        record["channel_sentinel_order"][2],
        record["channel_sentinel_order"][1],
    )
    with pytest.raises(PerfRecordError, match="sentinel order"):
        validate_record(record)


def test_host_record_cannot_claim_physical_sd_or_target_measurements():
    for field, value in (("sd_card_frequency_hz", 20_000_000), ("internal_heap_free_bytes", 160_000)):
        record = valid_record()
        record[field] = value
        with pytest.raises(PerfRecordError, match="HOST_VIRTUAL"):
            validate_record(record)


def test_unsupported_workload_cannot_silently_degrade():
    record = valid_record()
    record["workload_supported"] = False
    with pytest.raises(PerfRecordError, match="silently degraded"):
        validate_record(record)


def test_timing_schema_rejects_average_only_comparisons():
    record = valid_record()
    record["recorder_write_latency_us"]["average"] = 150
    with pytest.raises(PerfRecordError, match="exactly p50, p95, p99, max"):
        validate_record(record)


def test_profile_sha_and_surface_comparison_boundaries():
    old = valid_record()
    old.update(build_profile="DEBUG", optimization_mode="debug", firmware_sha256="a" * 64)
    new = copy.deepcopy(old)
    new.update(build_profile="PERFORMANCE", optimization_mode="performance", firmware_sha256="b" * 64)
    with pytest.raises(PerfRecordError, match="explicit A/B"):
        compare_records(old, new)
    assert compare_records(old, new, allow_ab=True)["ab_comparison"] is True

    live = copy.deepcopy(old)
    live.update(test_surface="LIVE_HIL", sd_card_identity="SDHC-test-01", sd_card_frequency_hz=20_000_000)
    with pytest.raises(PerfRecordError, match="cross-surface"):
        compare_records(old, live)


def test_profile_cannot_be_claimed_without_its_max361_firmware_sha():
    record = valid_record()
    record.update(build_profile="DEBUG", optimization_mode="debug")
    with pytest.raises(PerfRecordError, match="firmware SHA and build profile"):
        validate_record(record)


def test_baseline_update_requires_a_reason_and_a_passing_old_baseline():
    old = valid_record()
    new = valid_record()
    receipt = prepare_baseline_update(old, new, "MAX-369 approved workload change")
    assert receipt["old"] == old and receipt["new"] == new
    assert receipt["reason"] == "MAX-369 approved workload change"

    with pytest.raises(PerfRecordError, match="non-empty reason"):
        prepare_baseline_update(old, new, " ")
    old["active_allocation_count"] = 1
    with pytest.raises(PerfRecordError, match="active allocation count"):
        prepare_baseline_update(old, new, "attempted baseline regeneration")


def test_max361_build_evidence_is_the_only_profile_sha_source(tmp_path):
    evidence = tmp_path / "build-evidence.debug.json"
    evidence.write_text(
        json.dumps({"profile": "debug", "firmware_sha256": "c" * 64}),
        encoding="utf-8",
    )
    record = attach_build_evidence(valid_record(), evidence)
    assert record["build_profile"] == "DEBUG"
    assert record["optimization_mode"] == "debug"
    assert record["firmware_sha256"] == "c" * 64
    validate_record(record)


def test_virtual_stress_target_emits_valid_records_under_test_build_lock():
    assert BUILD_ROOT == REPO_ROOT / ".test-build"
    unique = uuid.uuid4().hex
    build_dir = BUILD_ROOT / f"perf-record-host-{unique}"
    log_path = BUILD_ROOT / f"perf-record-host-{unique}.log"
    command = [
        sys.executable,
        str(HOST_RUNNER),
        "--manifest",
        str(REPO_ROOT / "tests" / "host" / "manifest.d" / "performance-acceptance-foundation.json"),
        "--only",
        "virtual_board_stress",
        "--build-dir",
        str(build_dir),
    ]
    with host_compile_lock():
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=900,
            check=False,
            env=os.environ.copy(),
        )
    output = result.stdout + "\n" + result.stderr
    log_path.write_text(output, encoding="utf-8")
    assert result.returncode == 0, f"host stress exit {result.returncode}; see {log_path}"
    report = json.loads((build_dir / "host-test-report.json").read_text(encoding="utf-8"))
    stress_output = report["results"][0]["stdout"]
    output += stress_output
    log_path.write_text(output, encoding="utf-8")
    assert "VIRTUAL_BOARD_STRESS_RESULT: PASS physical_io=NONE com_ports=NONE" in stress_output

    records = [
        json.loads(line[len(PERF_PREFIX):])
        for line in stress_output.splitlines()
        if line.startswith(PERF_PREFIX)
    ]
    assert len(records) == 43
    for record in records:
        validate_record(record)
        assert record["produced_frames"] == (
            record["persisted_frames"]
            + record["explicitly_dropped_frames"]
            + record["explicitly_rejected_frames"]
            + record["resident_frames"]
        )
    by_workload = {record["workload_id"]: record for record in records}
    expected = {
        "healthy", "sd_jitter", "stall_below_batch_interval", "stall_around_batch_interval",
        "stall_above_batch_interval", "tft_contention", "wifi_activity", "i2c_activity",
        "worst_case_admitted", "intentional_saturation", "storage_saturation_500hz",
        "healthy_10000hz", "healthy_20000hz", "healthy_40000hz",
        "sd_jitter_5000hz", "sd_jitter_10000hz", "sd_jitter_20000hz",
        "sd_jitter_40000hz", "sd_stall_200ms_5000hz",
        "sd_stall_200ms_10000hz", "sd_stall_200ms_20000hz",
        "sd_stall_200ms_40000hz", "stall_below_batch_interval_5000hz",
        "stall_below_batch_interval_10000hz",
        "stall_below_batch_interval_20000hz",
        "stall_below_batch_interval_40000hz", "stall_above_batch_interval_5000hz",
        "stall_above_batch_interval_10000hz",
        "stall_above_batch_interval_20000hz",
        "stall_above_batch_interval_40000hz", "storage_saturation_10000hz",
        "storage_saturation_20000hz", "storage_saturation_40000hz",
        "tft_wifi_i2c_10000hz", "tft_wifi_i2c_20000hz", "tft_wifi_i2c_40000hz",
        "spi3_minimum_clock_10000hz_saturated_deadline_missed",
        "spi3_minimum_clock_20000hz_saturated_deadline_missed",
        "spi3_minimum_clock_40000hz_saturated_deadline_missed",
        "spi3_maximum_clock_40000hz_margin_control",
    }
    assert expected <= set(by_workload)
    assert by_workload["healthy"]["explicitly_dropped_frames"] == 0
    assert by_workload["intentional_saturation"]["classification"] == "SATURATED"
