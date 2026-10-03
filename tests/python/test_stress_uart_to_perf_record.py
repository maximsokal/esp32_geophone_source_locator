"""Parser contract tests for the ESP32-S3 target stress UART format."""

from __future__ import annotations

from pathlib import Path

import pytest

from tools.stress_uart_to_perf_record import (
    NOT_APPLICABLE,
    StressUARTParseError,
    parse_uart_log,
)
from tools.validate_perf_record import PerfRecordError, compare_records, validate_record


FIXTURE = Path(__file__).parent / "data" / "SYNTHETIC_target_stress_uart.log"
SD_IDENTITY = "owner-measured-SDHC-card-A"
SD_FREQUENCY_HZ = 20_000_000


def _evidence(tmp_path: Path, profile: str, sha: str) -> Path:
    path = tmp_path / f"build-evidence.{profile}.json"
    path.write_text(
        '{"profile":"' + profile + '","firmware_sha256":"' + sha + '"}\n',
        encoding="utf-8",
    )
    return path


def _records(text: str | None = None, *, evidence: Path, card: str = SD_IDENTITY):
    return parse_uart_log(
        FIXTURE.read_text(encoding="utf-8") if text is None else text,
        build_evidence_path=evidence,
        sd_card_identity=card,
        sd_card_frequency_hz=SD_FREQUENCY_HZ,
    )


def test_synthetic_fixture_yields_schema_valid_record_per_scenario_and_rate(tmp_path):
    evidence = _evidence(tmp_path, "performance", "a" * 64)
    records = _records(evidence=evidence)

    assert len(records) == 20
    for record in records:
        validate_record(record)
        assert record["test_surface"] == "TARGET_STRESS"
        assert record["build_profile"] == "PERFORMANCE"
        assert record["optimization_mode"] == "performance"
        assert record["sd_card_identity"] == SD_IDENTITY
        assert record["sd_card_frequency_hz"] == SD_FREQUENCY_HZ
        assert record["acquisition_service_jitter_us"] == NOT_APPLICABLE
        assert record["recorder_write_latency_us"] == NOT_APPLICABLE
        assert record["seismic_buffer_atomic_operations"] == NOT_APPLICABLE

    healthy = next(row for row in records if row["workload_id"] == "rate_sweep_100hz")
    assert healthy["produced_frames"] == 100
    assert healthy["ingested_frames"] == 100
    assert healthy["persisted_frames"] == 0
    assert healthy["explicitly_dropped_frames"] == 0
    assert healthy["explicitly_rejected_frames"] == 0
    assert healthy["resident_frames"] == 100
    assert healthy["internal_heap_free_bytes"] == 99000
    assert healthy["internal_heap_largest_block_bytes"] == 59000
    assert healthy["psram_free_bytes"] == 2999000
    assert healthy["psram_largest_block_bytes"] == 1999000
    assert healthy["stack_hwm_words"] == 768
    assert healthy["sample_format"] == NOT_APPLICABLE
    assert healthy["channel_count"] == NOT_APPLICABLE

    saturated = next(row for row in records if row["workload_id"] == "rate_sweep_5000hz")
    assert saturated["classification"] == "SATURATED"
    assert saturated["explicitly_dropped_frames"] == 150
    assert saturated["explicitly_rejected_frames"] == 50
    assert saturated["resident_frames"] == 4800

    # Binary-search saturation is an intentional boundary observation. The
    # ordinary rate-sweep negative control below still rejects unexpected loss.
    boundary = next(row for row in records if row["workload_id"] == "binary_search_4375hz")
    assert boundary["classification"] == "SATURATED"


def test_unexpected_lost_frame_is_rejected(tmp_path):
    evidence = _evidence(tmp_path, "debug", "b" * 64)
    text = FIXTURE.read_text(encoding="utf-8")
    line = next(line for line in text.splitlines() if "name=rate_sweep " in line and "rate_hz=100 " in line)
    text = text.replace(line, line.replace("ring_dropped=0", "ring_dropped=1", 1), 1)

    with pytest.raises(StressUARTParseError, match="counters disagree with observed outcome"):
        _records(text, evidence=evidence)


@pytest.mark.parametrize(
    "marker",
    [
        "PROJECT_HOT_PATH_ALLOCATION_VIOLATION count=1",
        "E (100) task_wdt: Task watchdog got triggered",
        "Brownout detector was triggered",
        "UNEXPECTED_RESET source=software",
    ],
)
def test_hard_failure_markers_are_rejected(tmp_path, marker):
    evidence = _evidence(tmp_path, "performance", "c" * 64)
    with pytest.raises(StressUARTParseError, match="hard failure marker"):
        _records(FIXTURE.read_text(encoding="utf-8") + marker + "\n", evidence=evidence)


def test_missing_mandatory_uart_field_is_rejected(tmp_path):
    evidence = _evidence(tmp_path, "performance", "d" * 64)
    text = FIXTURE.read_text(encoding="utf-8")
    line = next(line for line in text.splitlines() if "name=rate_sweep " in line and "rate_hz=100 " in line)
    text = text.replace(line, line.replace("produced=100 ", "", 1), 1)

    with pytest.raises(StressUARTParseError, match="fields invalid: missing produced"):
        _records(text, evidence=evidence)


def test_truncated_uart_capture_without_final_summary_is_rejected(tmp_path):
    evidence = _evidence(tmp_path, "performance", "e" * 64)
    text = FIXTURE.read_text(encoding="utf-8").rsplit("TARGET_STRESS_SUMMARY:", 1)[0]

    with pytest.raises(StressUARTParseError, match="exactly one TARGET_STRESS_BEGIN and final summary"):
        _records(text, evidence=evidence)


def test_target_record_requires_build_and_physical_sd_metadata(tmp_path):
    text = FIXTURE.read_text(encoding="utf-8")
    with pytest.raises(StressUARTParseError, match="MAX-361 build-evidence"):
        parse_uart_log(
            text,
            build_evidence_path=None,
            sd_card_identity=SD_IDENTITY,
            sd_card_frequency_hz=SD_FREQUENCY_HZ,
        )

    evidence = _evidence(tmp_path, "debug", "f" * 64)
    with pytest.raises(StressUARTParseError, match="SD card identity"):
        parse_uart_log(
            text,
            build_evidence_path=evidence,
            sd_card_identity=None,
            sd_card_frequency_hz=SD_FREQUENCY_HZ,
        )


def test_debug_and_performance_build_evidence_cannot_be_mixed(tmp_path):
    text = FIXTURE.read_text(encoding="utf-8")
    debug = _records(text, evidence=_evidence(tmp_path, "debug", "1" * 64))[0]
    performance = _records(text, evidence=_evidence(tmp_path, "performance", "2" * 64))[0]
    assert (debug["build_profile"], debug["firmware_sha256"]) == ("DEBUG", "1" * 64)
    assert (performance["build_profile"], performance["firmware_sha256"]) == ("PERFORMANCE", "2" * 64)

    with pytest.raises(PerfRecordError, match="profile or firmware SHA differs"):
        compare_records(debug, performance)
