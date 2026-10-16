#!/usr/bin/env python3
"""Convert complete ESP32-S3 stress-app UART captures to v2 TARGET_STRESS records."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.validate_perf_record import (  # noqa: E402
    NOT_APPLICABLE,
    PROPERTIES,
    PerfRecordError,
    attach_build_evidence,
    validate_record,
)


RATE_SWEEP = (100, 250, 500, 1000, 2000, 4000, 5000)
PHASE_FIELDS = {
    "name", "seed", "rate_hz", "duration_ms", "produced", "recorder",
    "ring_dropped", "cache_overflow", "producer_deadlines", "recorder_errors",
    "isr_ticks", "trace_events", "stack_prod", "stack_rec", "stack_ui",
    "stack_sensor", "stack_net", "stack_sched", "internal_start", "internal_end",
    "dma_start", "dma_end", "psram_start", "psram_end", "cache_psram",
    "recorder_batch_psram", "isr_timer", "task_create_failed", "observed",
    "expected", "verdict",
}
HEAP_FIELDS = {
    "stage", "internal_free", "dma_free", "psram_free", "internal_largest",
    "dma_largest", "psram_largest",
}
STACK_TASKS = ("producer", "recorder", "ui", "sensor", "network", "scheduler")
STACK_PHASE_FIELDS = {
    "producer": "stack_prod", "recorder": "stack_rec", "ui": "stack_ui",
    "sensor": "stack_sensor", "network": "stack_net", "scheduler": "stack_sched",
}
TRACE_FIELDS = {"seq", "event", "t_us", "jitter_us"}
ISR_FIELDS = {"period_us", "ticks", "expected_min", "status"}
SCHEDULE_FIELDS = {"seed", "events", "inversions", "trace_hash", "replay"}
NOT_APPLICABLE_LOG_FIELDS = (
    "DMA heap values, individual stack values beyond their minimum, ISR tick totals, "
    "trace event/jitter samples, and per-scenario deadline/error totals have no "
    "matching fields in performance-record schema v2; the raw UART log retains them."
)


class StressUARTParseError(ValueError):
    """A target UART capture is incomplete, inconsistent, or unsafe to record."""


def _key_values(line: str, marker: str, expected: set[str] | None = None) -> dict[str, str]:
    prefix = marker + " "
    if not line.startswith(prefix):
        raise StressUARTParseError(f"expected {marker} key/value line")
    fields: dict[str, str] = {}
    for token in line[len(prefix):].split():
        if "=" not in token:
            raise StressUARTParseError(f"malformed {marker} token: {token}")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise StressUARTParseError(f"malformed or duplicate {marker} field: {token}")
        fields[key] = value
    if expected is not None and set(fields) != expected:
        missing = sorted(expected - set(fields))
        extra = sorted(set(fields) - expected)
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unknown " + ", ".join(extra))
        raise StressUARTParseError(f"{marker} fields invalid: {'; '.join(details)}")
    return fields


def _uint(fields: dict[str, str], key: str, marker: str) -> int:
    value = fields[key]
    if not re.fullmatch(r"\d+", value):
        raise StressUARTParseError(f"{marker} {key} must be an unsigned decimal integer")
    return int(value, 10)


def _reject_hard_markers(text: str) -> None:
    hard_markers = (
        ("brownout", re.compile(r"BROWNOUT_RST|Brownout detector was triggered|E\s+BOD:\s*Brownout", re.I)),
        ("watchdog", re.compile(r"\b(?:watchdog|WDT|TG[01]WDT|RTCWDT|TASK_WDT)\b", re.I)),
        (
            "unexpected reset",
            re.compile(r"\b(?:UNEXPECTED_RESET|RESET_MARKER|RESET_DETECTED|ESP_RST_(?:SW|WDT|PANIC|BROWNOUT|CPU_LOCKUP))\b", re.I),
        ),
        (
            "panic/corruption",
            re.compile(r"Guru Meditation|CORRUPT HEAP|Stack smashing|abort\(\)|Backtrace:", re.I),
        ),
        (
            "allocation violation",
            re.compile(r"(?<![A-Z0-9])(?:hot[_ -]?path[_ -]+)?alloc(?:ation)?[_ -]?(?:violation|error|failure)(?![A-Z0-9])", re.I),
        ),
    )
    for name, pattern in hard_markers:
        match = pattern.search(text)
        if match:
            raise StressUARTParseError(f"hard failure marker ({name}): {match.group(0)}")

    for match in re.finditer(r"\brst:0x[0-9a-f]+\s*\(([^)]+)\)", text, re.I):
        cause = match.group(1).strip().upper()
        if cause not in {"POWERON_RESET", "EXT_CPU_RESET", "USB_UART_CHIP_RESET"}:
            raise StressUARTParseError(f"unexpected reset marker: {cause}")


def _empty_record() -> dict[str, Any]:
    record = {name: NOT_APPLICABLE for name in PROPERTIES}
    record["schema_version"] = 2
    record["test_surface"] = "TARGET_STRESS"
    record["reset_markers"] = {
        "unexpected_reset": False,
        "watchdog": False,
        "brownout": False,
    }
    return record


def _build_record(
    phase: dict[str, str],
    heap_start: dict[str, str],
    heap_end: dict[str, str],
    stack: dict[str, dict[str, str]],
    trace: list[dict[str, str]],
    isr: dict[str, str],
    binary_boundary: tuple[int, int],
    sd_card_identity: str,
    sd_card_frequency_hz: int,
) -> dict[str, Any]:
    for key in PHASE_FIELDS - {"name", "observed", "expected", "verdict"}:
        _uint(phase, key, "TARGET_STRESS_SCENARIO")
    name = phase["name"]
    rate = _uint(phase, "rate_hz", "TARGET_STRESS_SCENARIO")
    produced = _uint(phase, "produced", "TARGET_STRESS_SCENARIO")
    recorder = _uint(phase, "recorder", "TARGET_STRESS_SCENARIO")
    ring_dropped = _uint(phase, "ring_dropped", "TARGET_STRESS_SCENARIO")
    cache_overflow = _uint(phase, "cache_overflow", "TARGET_STRESS_SCENARIO")
    deadline_misses = _uint(phase, "producer_deadlines", "TARGET_STRESS_SCENARIO")
    recorder_errors = _uint(phase, "recorder_errors", "TARGET_STRESS_SCENARIO")
    task_create_failed = _uint(phase, "task_create_failed", "TARGET_STRESS_SCENARIO")
    isr_ticks = _uint(phase, "isr_ticks", "TARGET_STRESS_SCENARIO")
    duration_ms = _uint(phase, "duration_ms", "TARGET_STRESS_SCENARIO")
    seed = _uint(phase, "seed", "TARGET_STRESS_SCENARIO")
    trace_events = _uint(phase, "trace_events", "TARGET_STRESS_SCENARIO")

    if name not in {"rate_sweep", "binary_search", "intentional_saturation"}:
        raise StressUARTParseError(f"unsupported scenario name: {name}")
    if rate == 0 or duration_ms == 0 or produced == 0:
        raise StressUARTParseError(f"{name}/{rate}Hz has a zero rate, duration, or produced count")
    if trace_events == 0:
        raise StressUARTParseError(f"{name}/{rate}Hz emitted no scheduler/event trace")
    if task_create_failed or recorder_errors:
        raise StressUARTParseError(f"{name}/{rate}Hz reports task creation or recorder errors")
    if _uint(phase, "isr_timer", "TARGET_STRESS_SCENARIO") != 1:
        raise StressUARTParseError(f"{name}/{rate}Hz did not start the ISR timer")
    if isr["status"] != "PASS" or isr["period_us"] != "500":
        raise StressUARTParseError(f"{name}/{rate}Hz has a failed or unexpected ISR timing record")
    if _uint(isr, "ticks", "TARGET_STRESS_ISR") != isr_ticks:
        raise StressUARTParseError(f"{name}/{rate}Hz ISR summary disagrees with scenario ticks")
    if isr_ticks < _uint(isr, "expected_min", "TARGET_STRESS_ISR"):
        raise StressUARTParseError(f"{name}/{rate}Hz ISR ticks are below expected_min")

    expected_saturated = (
        ring_dropped > 0 or cache_overflow > 0 or deadline_misses > 0 or recorder_errors > 0
    )
    observed = "SATURATED" if expected_saturated else "PASS"
    if phase["observed"] != observed:
        raise StressUARTParseError(f"{name}/{rate}Hz counters disagree with observed outcome")
    if name == "binary_search":
        healthy_rate, first_failed_rate = binary_boundary
        expected_observed = "PASS" if rate <= healthy_rate else "SATURATED"
        expected_verdict = "PASS" if expected_observed == "PASS" else "FAIL"
        if (
            phase["expected"] != "PASS"
            or phase["observed"] != expected_observed
            or phase["verdict"] != expected_verdict
            or not (rate <= healthy_rate or rate >= first_failed_rate)
        ):
            raise StressUARTParseError(f"{name}/{rate}Hz disagrees with binary-search boundary")
    else:
        expected = "SATURATED" if (name == "intentional_saturation" or rate == 5000) else "PASS"
        if phase["expected"] != expected or observed != expected or phase["verdict"] != "PASS":
            raise StressUARTParseError(f"{name}/{rate}Hz did not meet expected outcome {expected}")

    expected_trace_count = min(trace_events, 16)
    trace_sequences = [_uint(row, "seq", "TARGET_STRESS_TRACE") for row in trace]
    first_trace_sequence = max(0, trace_events - 16)
    if trace_sequences != list(range(first_trace_sequence, trace_events)):
        raise StressUARTParseError(f"{name}/{rate}Hz trace sequence/count is incomplete")
    if len(trace) != expected_trace_count:
        raise StressUARTParseError(f"{name}/{rate}Hz expected {expected_trace_count} trace records, got {len(trace)}")

    expected_stack = {
        task: _uint(phase, value_field, "TARGET_STRESS_SCENARIO")
        for task, value_field in STACK_PHASE_FIELDS.items()
    }
    if set(stack) != set(STACK_TASKS):
        raise StressUARTParseError(f"{name}/{rate}Hz is missing a task stack HWM line")
    for task, summary_value in expected_stack.items():
        if _uint(stack[task], "free_bytes", "TARGET_STRESS_STACK") != summary_value:
            raise StressUARTParseError(f"{name}/{rate}Hz {task} stack summary disagrees with stack marker")

    expected_heap_values = {
        "start": {
            "internal_free": _uint(phase, "internal_start", "TARGET_STRESS_SCENARIO"),
            "dma_free": _uint(phase, "dma_start", "TARGET_STRESS_SCENARIO"),
            "psram_free": _uint(phase, "psram_start", "TARGET_STRESS_SCENARIO"),
        },
        "end": {
            "internal_free": _uint(phase, "internal_end", "TARGET_STRESS_SCENARIO"),
            "dma_free": _uint(phase, "dma_end", "TARGET_STRESS_SCENARIO"),
            "psram_free": _uint(phase, "psram_end", "TARGET_STRESS_SCENARIO"),
        },
    }
    for stage, values in expected_heap_values.items():
        snapshot = heap_start if stage == "start" else heap_end
        for key, value in values.items():
            if _uint(snapshot, key, "TARGET_STRESS_HEAP") != value:
                raise StressUARTParseError(f"{name}/{rate}Hz {stage} heap summary disagrees on {key}")

    # Cache persistence is only simulated in RAM (the phase waits and marks a
    # cache range); this stress workload does not write scenario frames to SD.
    # Keep persistent frames at zero and account remaining produced frames as resident.
    persisted = 0
    dropped = ring_dropped
    rejected = cache_overflow
    resident = produced - persisted - dropped - rejected
    if resident < 0 or recorder > produced - dropped - rejected:
        raise StressUARTParseError(f"{name}/{rate}Hz frame accounting exceeds produced frames")

    record = _empty_record()
    record.update({
        "sample_rate_hz": rate,
        "channel_count": NOT_APPLICABLE,
        "sample_format": NOT_APPLICABLE,
        "channel_sentinel_order": NOT_APPLICABLE,
        "sd_card_identity": sd_card_identity,
        "sd_card_frequency_hz": sd_card_frequency_hz,
        "workload_id": f"{name}_{rate}hz",
        "seed": seed,
        "duration_ms": duration_ms,
        "classification": observed,
        "produced_frames": produced,
        "ingested_frames": recorder,
        "persisted_frames": persisted,
        "explicitly_dropped_frames": dropped,
        "explicitly_rejected_frames": rejected,
        "resident_frames": resident,
        "internal_heap_free_bytes": _uint(heap_end, "internal_free", "TARGET_STRESS_HEAP"),
        "internal_heap_largest_block_bytes": _uint(heap_end, "internal_largest", "TARGET_STRESS_HEAP"),
        "psram_free_bytes": _uint(heap_end, "psram_free", "TARGET_STRESS_HEAP"),
        "psram_largest_block_bytes": _uint(heap_end, "psram_largest", "TARGET_STRESS_HEAP"),
        "stack_hwm_bytes": min(expected_stack.values()),
        "reset_markers": {"unexpected_reset": False, "watchdog": False, "brownout": False},
        "deadlock_detected": False,
    })
    return record


def parse_uart_log(
    text: str,
    *,
    build_evidence_path: str | Path | None,
    sd_card_identity: str | None,
    sd_card_frequency_hz: int | None,
) -> list[dict[str, Any]]:
    """Return one validated TARGET_STRESS record per full scenario/rate in a capture."""

    if not build_evidence_path:
        raise StressUARTParseError("MAX-361 build-evidence JSON is required for a valid TARGET_STRESS record")
    if not isinstance(sd_card_identity, str) or not sd_card_identity.strip():
        raise StressUARTParseError("physical SD card identity is required; it is not printed by the stress app")
    if type(sd_card_frequency_hz) is not int or sd_card_frequency_hz <= 0:
        raise StressUARTParseError("physical SD card frequency in Hz is required; it is not printed by the stress app")

    _reject_hard_markers(text)
    lines = [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    begin_lines = [line for line in lines if line.startswith("TARGET_STRESS_BEGIN ")]
    summary_lines = [line for line in lines if line.startswith("TARGET_STRESS_SUMMARY:")]
    if len(begin_lines) != 1 or len(summary_lines) != 1:
        raise StressUARTParseError("capture must contain exactly one TARGET_STRESS_BEGIN and final summary")

    begin = _key_values(begin_lines[0], "TARGET_STRESS_BEGIN", {"seed", "core_count"})
    seed = _uint(begin, "seed", "TARGET_STRESS_BEGIN")
    if _uint(begin, "core_count", "TARGET_STRESS_BEGIN") != 2:
        raise StressUARTParseError("TARGET_STRESS_BEGIN is not a dual-core ESP32-S3 capture")
    summary_match = re.fullmatch(r"TARGET_STRESS_SUMMARY:\s*(PASS|FAIL)\s+seed=(\d+)", summary_lines[0])
    if not summary_match or summary_match.group(1) != "PASS":
        raise StressUARTParseError("target stress summary is missing, malformed, or FAIL")
    if int(summary_match.group(2)) != seed:
        raise StressUARTParseError("TARGET_STRESS_SUMMARY seed disagrees with TARGET_STRESS_BEGIN")
    if lines.index(summary_lines[0]) < lines.index(begin_lines[0]):
        raise StressUARTParseError("target stress summary precedes application identity")

    if lines.count("TARGET_STRESS_PHYSICAL_BEGIN") != 1 or lines.count("TARGET_STRESS_PHYSICAL_END") != 1:
        raise StressUARTParseError("physical probe begin/end markers are incomplete")
    sd_probe = None
    for line in lines:
        if line.startswith("TARGET_STRESS_PHYSICAL "):
            fields = _key_values(line, "TARGET_STRESS_PHYSICAL")
            if fields.get("name") == "spi2_sd":
                if sd_probe is not None:
                    raise StressUARTParseError("duplicate SPI2/SD physical probe line")
                if set(fields) != {"name", "spi2", "iterations", "sd_mount", "sd_rw_passes", "verdict"}:
                    raise StressUARTParseError("SPI2/SD physical probe fields are incomplete or unknown")
                sd_probe = fields
    if (
        sd_probe is None
        or sd_probe.get("sd_mount") != "PASS"
        or sd_probe.get("verdict") != "PASS"
        or not sd_probe.get("sd_rw_passes", "0").isdigit()
        or int(sd_probe.get("sd_rw_passes", "0")) <= 0
    ):
        raise StressUARTParseError("physical SPI2/SD probe did not report a passing write/read check")

    binary_lines = [line for line in lines if line.startswith("TARGET_STRESS_BINARY_SEARCH ")]
    if len(binary_lines) != 1:
        raise StressUARTParseError("capture must contain exactly one binary-search boundary")
    binary = _key_values(binary_lines[0], "TARGET_STRESS_BINARY_SEARCH", {"seed", "healthy_rate_hz", "first_failed_rate_hz"})
    if _uint(binary, "seed", "TARGET_STRESS_BINARY_SEARCH") != seed:
        raise StressUARTParseError("binary-search seed disagrees with application identity")
    healthy_rate = _uint(binary, "healthy_rate_hz", "TARGET_STRESS_BINARY_SEARCH")
    first_failed_rate = _uint(binary, "first_failed_rate_hz", "TARGET_STRESS_BINARY_SEARCH")
    if first_failed_rate != healthy_rate + 1:
        raise StressUARTParseError("binary-search healthy/failed boundary must be adjacent rates")

    pending_heap: dict[str, dict[str, str]] = {}
    pending_stack: dict[str, dict[str, str]] = {}
    pending_trace: list[dict[str, str]] = []
    pending_isr: dict[str, str] | None = None
    pending_schedule: dict[str, str] | None = None
    parsed: list[dict[str, Any]] = []
    for line in lines:
        if line.startswith("TARGET_STRESS_SCHEDULE "):
            schedule = _key_values(line, "TARGET_STRESS_SCHEDULE", SCHEDULE_FIELDS)
            if _uint(schedule, "seed", "TARGET_STRESS_SCHEDULE") != seed:
                raise StressUARTParseError("schedule seed disagrees with application identity")
            if (
                _uint(schedule, "events", "TARGET_STRESS_SCHEDULE") < 64
                or _uint(schedule, "inversions", "TARGET_STRESS_SCHEDULE") == 0
                or not re.fullmatch(r"\d+", schedule["trace_hash"])
                or schedule["replay"] != "PASS"
            ):
                raise StressUARTParseError("schedule replay/inversion check failed")
            pending_schedule = schedule
        elif line.startswith("TARGET_STRESS_HEAP "):
            heap = _key_values(line, "TARGET_STRESS_HEAP", HEAP_FIELDS)
            if heap["stage"] not in ("start", "end") or heap["stage"] in pending_heap:
                raise StressUARTParseError("duplicate or unknown heap snapshot stage")
            for key in HEAP_FIELDS - {"stage"}:
                _uint(heap, key, "TARGET_STRESS_HEAP")
            pending_heap[heap["stage"]] = heap
        elif line.startswith("TARGET_STRESS_STACK "):
            stack = _key_values(line, "TARGET_STRESS_STACK")
            if set(stack) not in (
                {"task", "core", "free_bytes"},
                {"task", "core", "free_words"},
            ):
                raise StressUARTParseError("TARGET_STRESS_STACK needs task, core, and one HWM field")
            if stack["task"] not in STACK_TASKS or stack["task"] in pending_stack:
                raise StressUARTParseError("duplicate or unknown stack task marker")
            _uint(stack, "core", "TARGET_STRESS_STACK")
            hwm_field = "free_bytes" if "free_bytes" in stack else "free_words"
            stack["free_bytes"] = stack.pop(hwm_field)
            _uint(stack, "free_bytes", "TARGET_STRESS_STACK")
            pending_stack[stack["task"]] = stack
        elif line.startswith("TARGET_STRESS_TRACE "):
            trace = _key_values(line, "TARGET_STRESS_TRACE", TRACE_FIELDS)
            for key in TRACE_FIELDS:
                _uint(trace, key, "TARGET_STRESS_TRACE")
            pending_trace.append(trace)
        elif line.startswith("TARGET_STRESS_ISR "):
            if pending_isr is not None:
                raise StressUARTParseError("duplicate ISR marker before scenario summary")
            pending_isr = _key_values(line, "TARGET_STRESS_ISR", ISR_FIELDS)
            for key in ("period_us", "ticks", "expected_min"):
                _uint(pending_isr, key, "TARGET_STRESS_ISR")
        elif line.startswith("TARGET_STRESS_SCENARIO "):
            phase = _key_values(line, "TARGET_STRESS_SCENARIO", PHASE_FIELDS)
            if pending_schedule is None:
                raise StressUARTParseError("scenario has no preceding passing schedule probe")
            if _uint(phase, "seed", "TARGET_STRESS_SCENARIO") != seed:
                raise StressUARTParseError("scenario seed disagrees with application identity")
            if set(pending_heap) != {"start", "end"}:
                raise StressUARTParseError("scenario is missing a start/end heap snapshot")
            if set(pending_stack) != set(STACK_TASKS):
                raise StressUARTParseError("scenario is missing one or more task stack HWM lines")
            if pending_isr is None:
                raise StressUARTParseError("scenario is missing ISR timing marker")
            if _uint(pending_schedule, "seed", "TARGET_STRESS_SCHEDULE") != seed:
                raise StressUARTParseError("scenario schedule seed mismatch")
            record = _build_record(
                phase,
                pending_heap["start"],
                pending_heap["end"],
                pending_stack,
                pending_trace,
                pending_isr,
                (healthy_rate, first_failed_rate),
                sd_card_identity.strip(),
                sd_card_frequency_hz,
            )
            parsed.append(record)
            pending_heap = {}
            pending_stack = {}
            pending_trace = []
            pending_isr = None
            pending_schedule = None

    actual_ids = [record["workload_id"] for record in parsed]
    sweep_ids = [f"rate_sweep_{rate}hz" for rate in RATE_SWEEP]
    if (
        len(actual_ids) != len(set(actual_ids))
        or actual_ids[:len(sweep_ids)] != sweep_ids
        or len(actual_ids) < len(sweep_ids) + 2
        or actual_ids[-1] != "intentional_saturation_5000hz"
        or any(not item.startswith("binary_search_") for item in actual_ids[len(sweep_ids):-1])
    ):
        raise StressUARTParseError("capture is truncated or missing/duplicating expected scenario/rate records")
    binary_phase_lines = [
        index for index, line in enumerate(lines)
        if line.startswith("TARGET_STRESS_SCENARIO ") and "name=binary_search " in line
    ]
    if not binary_phase_lines or lines.index(binary_lines[0]) < max(binary_phase_lines):
        raise StressUARTParseError("binary-search result must follow its scenario records")
    if lines.index(summary_lines[0]) < lines.index(binary_lines[0]):
        raise StressUARTParseError("final summary precedes binary-search result")
    scenario_lines = [index for index, line in enumerate(lines) if line.startswith("TARGET_STRESS_SCENARIO ")]
    if not scenario_lines or lines.index(summary_lines[0]) < max(scenario_lines):
        raise StressUARTParseError("final summary precedes the final scenario")
    if pending_heap or pending_stack or pending_trace or pending_isr is not None or pending_schedule is not None:
        raise StressUARTParseError("capture ends with incomplete scenario telemetry")

    try:
        records = [attach_build_evidence(record, build_evidence_path) for record in parsed]
    except (OSError, json.JSONDecodeError, PerfRecordError) as exc:
        raise StressUARTParseError(f"cannot attach MAX-361 build evidence: {exc}") from exc

    identities = {(row["firmware_sha256"], row["build_profile"]) for row in records}
    if len(identities) != 1:
        raise StressUARTParseError("one UART capture cannot mix DEBUG/PERFORMANCE build evidence")
    for record in records:
        try:
            validate_record(record)
        except PerfRecordError as exc:
            raise StressUARTParseError(f"invalid TARGET_STRESS record {record['workload_id']}: {exc}") from exc
    return records


def _write_jsonl(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(
            "".join(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n" for record in records),
            encoding="utf-8",
        )
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", action="append", required=True, help="UART capture file; repeat to include monitor stderr")
    parser.add_argument("--build-evidence", help="MAX-361 build-evidence.<profile>.json for this firmware")
    parser.add_argument("--sd-card-identity", help="Operator-measured physical SD identity (required by TARGET_STRESS schema)")
    parser.add_argument("--sd-card-frequency-hz", type=int, help="Physical SD frequency in Hz (required by TARGET_STRESS schema)")
    parser.add_argument("--output", required=True, help="Destination JSONL; one record per scenario/rate")
    args = parser.parse_args(argv)
    try:
        texts = [Path(path).read_text(encoding="utf-8", errors="replace") for path in args.log]
        records = parse_uart_log(
            "\n".join(texts),
            build_evidence_path=args.build_evidence,
            sd_card_identity=args.sd_card_identity,
            sd_card_frequency_hz=args.sd_card_frequency_hz,
        )
        output = Path(args.output)
        _write_jsonl(output, records)
    except (OSError, PerfRecordError, StressUARTParseError, json.JSONDecodeError) as exc:
        print(f"STRESS_UART_PARSE_ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"STRESS_UART_RECORDS: {len(records)}")
    print(f"STRESS_UART_RECORD_FILE: {output}")
    print(f"STRESS_UART_NOT_REPRESENTED: {NOT_APPLICABLE_LOG_FIELDS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
