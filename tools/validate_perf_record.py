#!/usr/bin/env python3
"""Strict validation and same-surface comparison for performance records."""

from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, Mapping


SCHEMA_PATH = Path(__file__).with_name("perf_record_schema.json")
SCHEMA = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
REQUIRED_FIELDS = set(SCHEMA["required"])
PROPERTIES = SCHEMA["properties"]
NOT_APPLICABLE = "not_applicable"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
FRAME_COUNTS = (
    "produced_frames",
    "ingested_frames",
    "persisted_frames",
    "explicitly_dropped_frames",
    "explicitly_rejected_frames",
    "resident_frames",
)
TIMING_FIELDS = ("acquisition_service_jitter_us", "recorder_write_latency_us")
PERCENTILES = ("p50", "p95", "p99", "max")


class PerfRecordError(ValueError):
    """A record, comparison, or baseline update violates the acceptance contract."""


def _is_count(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _is_number(value: object) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return value >= 0
    return isinstance(value, float) and math.isfinite(value) and value >= 0


def _validate_metric(value: object, kind: str, name: str, errors: list[str]) -> None:
    if value == NOT_APPLICABLE:
        return
    valid = _is_count(value) if kind == "count" else _is_number(value)
    if not valid:
        errors.append(f"{name} must be a non-negative {kind} or {NOT_APPLICABLE}")


def record_errors(record: object) -> list[str]:
    """Return every strict schema and hard-invariant failure without mutating input."""

    if not isinstance(record, Mapping):
        return ["record must be a JSON object"]

    errors: list[str] = []
    missing = REQUIRED_FIELDS - set(record)
    extra = set(record) - REQUIRED_FIELDS
    if missing:
        errors.append("missing fields: " + ", ".join(sorted(missing)))
    if extra:
        errors.append("unknown fields: " + ", ".join(sorted(extra)))
    if missing or extra:
        return errors

    if type(record["schema_version"]) is not int or record["schema_version"] != 1:
        errors.append("schema_version must be integer 1")

    sha = record["firmware_sha256"]
    if sha != NOT_APPLICABLE and (not isinstance(sha, str) or not SHA256.fullmatch(sha)):
        errors.append("firmware_sha256 must be 64 lowercase hex characters or not_applicable")

    profile = record["build_profile"]
    optimization = record["optimization_mode"]
    if profile not in ("DEBUG", "PERFORMANCE", NOT_APPLICABLE):
        errors.append("build_profile must be DEBUG, PERFORMANCE, or not_applicable")
    if optimization not in ("debug", "performance", NOT_APPLICABLE):
        errors.append("optimization_mode must be debug, performance, or not_applicable")
    if profile == "DEBUG" and optimization != "debug":
        errors.append("DEBUG profile must use debug optimization")
    if profile == "PERFORMANCE" and optimization != "performance":
        errors.append("PERFORMANCE profile must use performance optimization")
    if (profile == NOT_APPLICABLE) != (optimization == NOT_APPLICABLE):
        errors.append("profile and optimization metadata must both be explicit or both be not_applicable")
    if (profile == NOT_APPLICABLE) != (sha == NOT_APPLICABLE):
        errors.append("firmware SHA and build profile must both be explicit or both be not_applicable")

    surface = record["test_surface"]
    if surface not in ("HOST_VIRTUAL", "TARGET_STRESS", "LIVE_HIL"):
        errors.append("test_surface must be HOST_VIRTUAL, TARGET_STRESS, or LIVE_HIL")
    elif surface != "HOST_VIRTUAL":
        if sha == NOT_APPLICABLE or profile == NOT_APPLICABLE:
            errors.append("target and live records require MAX-361 firmware profile evidence")
        if record["sd_card_identity"] == NOT_APPLICABLE or record["sd_card_frequency_hz"] == NOT_APPLICABLE:
            errors.append("physical records require SD card identity and frequency")

    if not isinstance(record["sample_format"], str) or not record["sample_format"]:
        errors.append("sample_format must be a non-empty string or not_applicable")
    if record["sample_format"] == NOT_APPLICABLE:
        pass
    for name in ("sample_rate_hz", "channel_count"):
        value = record[name]
        if value != NOT_APPLICABLE and (not _is_count(value) or value == 0):
            errors.append(f"{name} must be a positive integer or not_applicable")
    if not isinstance(record["sd_card_identity"], str) or not record["sd_card_identity"]:
        errors.append("sd_card_identity must be a non-empty string or not_applicable")
    if record["sd_card_frequency_hz"] != NOT_APPLICABLE and (
        not _is_count(record["sd_card_frequency_hz"]) or record["sd_card_frequency_hz"] == 0
    ):
        errors.append("sd_card_frequency_hz must be a positive integer or not_applicable")
    channel_order = record["channel_sentinel_order"]
    expected_order = ["frame", "negative_frame", "frame_plus_quarter", "frame_minus_quarter"]
    if channel_order != NOT_APPLICABLE and (
        not isinstance(channel_order, list)
        or len(channel_order) != 4
        or not all(isinstance(item, str) for item in channel_order)
        or len(set(channel_order)) != 4
        or any(item not in expected_order for item in channel_order)
    ):
        errors.append("channel_sentinel_order must contain four unique channel sentinels or not_applicable")
    if (
        surface == "HOST_VIRTUAL"
        and record["sample_format"] == "float32_uv"
        and record["channel_count"] == 4
        and channel_order != expected_order
    ):
        errors.append("HOST_VIRTUAL float32_uv records must preserve channel sentinel order")
    if not isinstance(record["workload_id"], str) or not record["workload_id"]:
        errors.append("workload_id must be a non-empty string")
    if not _is_count(record["seed"]):
        errors.append("seed must be a non-negative integer")
    if not _is_count(record["duration_ms"]) or record["duration_ms"] == 0:
        errors.append("duration_ms must be a positive integer")
    if record["classification"] not in ("PASS", "SATURATED", "REJECTED"):
        errors.append("classification must be PASS, SATURATED, or REJECTED")

    for name in FRAME_COUNTS:
        if not _is_count(record[name]):
            errors.append(f"{name} must be a non-negative integer")
    if all(_is_count(record[name]) for name in FRAME_COUNTS):
        produced = record["produced_frames"]
        ingested = record["ingested_frames"]
        persisted = record["persisted_frames"]
        dropped = record["explicitly_dropped_frames"]
        rejected = record["explicitly_rejected_frames"]
        resident = record["resident_frames"]
        if produced != persisted + dropped + rejected + resident:
            errors.append("unaccounted frame loss: produced must equal persisted + dropped + rejected + resident")
        if persisted > ingested or ingested + dropped + rejected > produced:
            errors.append("frame counts violate produced/ingested/persisted ordering")

    for name in TIMING_FIELDS:
        value = record[name]
        if value == NOT_APPLICABLE:
            continue
        if not isinstance(value, Mapping) or set(value) != set(PERCENTILES):
            errors.append(f"{name} must contain exactly p50, p95, p99, max or be not_applicable")
            continue
        for percentile in PERCENTILES:
            if not _is_number(value[percentile]):
                errors.append(f"{name}.{percentile} must be a non-negative number")
        if all(_is_number(value[key]) for key in PERCENTILES):
            if not (value["p50"] <= value["p95"] <= value["p99"] <= value["max"]):
                errors.append(f"{name} percentiles must be monotonic through max")

    for name in ("bytes_per_write", "writes_per_second", "web_stop_latency_ms", "web_status_latency_ms", "live_latency_ms"):
        _validate_metric(record[name], "number", name, errors)
    count_fields = (
        "ring_high_water_frames", "cache_high_water_frames", "queue_high_water",
        "cpu_runtime_us", "task_runtime_us", "core_runtime_us", "internal_heap_free_bytes",
        "internal_heap_largest_block_bytes", "psram_free_bytes", "psram_largest_block_bytes",
        "stack_hwm_words", "project_hot_path_allocation_violations", "active_allocation_count",
        "diagnostic_reports_suppressed", "diagnostic_reports_coalesced", "v2_quantizer_calls",
        "seismic_buffer_atomic_operations", "seismic_buffer_critical_section_entries",
        "seismic_buffer_retries", "ad7606_status_critical_section_entries",
        "ad7606_status_critical_section_total_us", "ad7606_status_critical_section_max_us",
    )
    for name in count_fields:
        _validate_metric(record[name], "count", name, errors)

    reset_markers = record["reset_markers"]
    expected_markers = {"unexpected_reset", "watchdog", "brownout"}
    if not isinstance(reset_markers, Mapping) or set(reset_markers) != expected_markers:
        errors.append("reset_markers must contain unexpected_reset, watchdog, and brownout")
    else:
        for name, value in reset_markers.items():
            if not isinstance(value, bool) and value != NOT_APPLICABLE:
                errors.append(f"reset_markers.{name} must be boolean or not_applicable")
        if surface == "HOST_VIRTUAL" and any(value != NOT_APPLICABLE for value in reset_markers.values()):
            errors.append("HOST_VIRTUAL cannot claim physical reset, watchdog, or brownout markers")

    for name in (
        "frame_duplication_detected", "frame_reordering_detected", "channel_swap_detected",
        "corrupted_finalized_file", "corrupted_finalized_session", "dirty_cache_overwrite_detected",
        "diagnostic_integer_overflow_detected", "deadlock_detected",
    ):
        if not isinstance(record[name], bool) and record[name] != NOT_APPLICABLE:
            errors.append(f"{name} must be boolean or not_applicable")
    if not isinstance(record["workload_supported"], bool) and record["workload_supported"] != NOT_APPLICABLE:
        errors.append("workload_supported must be boolean or not_applicable")
    if record["raw_path_selection"] not in ("RAW", "QUANTIZED", NOT_APPLICABLE):
        errors.append("raw_path_selection must be RAW, QUANTIZED, or not_applicable")
    if record["unsupported_workload_disposition"] not in (NOT_APPLICABLE, "REJECTED", "SATURATED"):
        errors.append("unsupported_workload_disposition must explicitly classify rejection or saturation")

    for name, spec in PROPERTIES.items():
        if spec.get("x-host-virtual-must-be-na") and surface == "HOST_VIRTUAL" and record[name] != NOT_APPLICABLE:
            errors.append(f"HOST_VIRTUAL cannot claim {name}; it must be not_applicable")
    if surface == "HOST_VIRTUAL" and record["sd_card_frequency_hz"] != NOT_APPLICABLE:
        errors.append("HOST_VIRTUAL cannot claim physical SD card frequency")
    if surface == "HOST_VIRTUAL" and record["sd_card_identity"] != NOT_APPLICABLE:
        errors.append("HOST_VIRTUAL cannot claim physical SD card identity")

    hard_failure_flags = (
        "frame_duplication_detected", "frame_reordering_detected", "channel_swap_detected",
        "corrupted_finalized_file", "corrupted_finalized_session", "dirty_cache_overwrite_detected",
        "diagnostic_integer_overflow_detected", "deadlock_detected",
    )
    for name in hard_failure_flags:
        if record[name] is True:
            errors.append(f"hard failure: {name}")
    for name, value in reset_markers.items() if isinstance(reset_markers, Mapping) else ():
        if value is True:
            errors.append(f"hard failure: reset marker {name}")
    if _is_count(record["project_hot_path_allocation_violations"]) and record["project_hot_path_allocation_violations"] > 0:
        errors.append("hard failure: project hot-path allocation violation")
    if _is_count(record["active_allocation_count"]) and record["active_allocation_count"] > 0:
        errors.append("hard failure: active allocation count is nonzero")
    if record["workload_supported"] is False and record["unsupported_workload_disposition"] == NOT_APPLICABLE:
        errors.append("unsupported workload silently degraded without explicit rejection or saturation")
    if record["workload_supported"] is False and record["unsupported_workload_disposition"] != record["classification"]:
        errors.append("unsupported workload disposition must match record classification")
    return errors


def validate_record(record: object) -> None:
    """Raise PerfRecordError unless a record is complete and satisfies hard gates."""

    errors = record_errors(record)
    if errors:
        raise PerfRecordError("; ".join(errors))


def attach_build_evidence(record: Mapping[str, Any], evidence_path: str | Path) -> dict[str, Any]:
    """Fill profile/SHA only from the existing MAX-361 idf_profile evidence artifact."""

    evidence = json.loads(Path(evidence_path).read_text(encoding="utf-8"))
    if not isinstance(evidence, dict):
        raise PerfRecordError("build evidence must be a JSON object")
    raw_profile = evidence.get("profile")
    profile = raw_profile.upper() if isinstance(raw_profile, str) else ""
    sha = evidence.get("firmware_sha256")
    if profile not in ("DEBUG", "PERFORMANCE") or not isinstance(sha, str) or not SHA256.fullmatch(sha):
        raise PerfRecordError("MAX-361 build evidence must contain profile and firmware_sha256")

    result = copy.deepcopy(dict(record))
    supplied = {
        "firmware_sha256": sha,
        "build_profile": profile,
        "optimization_mode": profile.lower(),
    }
    for name, value in supplied.items():
        current = result.get(name, NOT_APPLICABLE)
        if current not in (NOT_APPLICABLE, value):
            raise PerfRecordError(f"record {name} disagrees with MAX-361 build evidence")
        result[name] = value
    return result


def compare_records(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any], *, allow_ab: bool = False
) -> dict[str, Any]:
    """Compare percentile timings only, refusing cross-surface/profile evidence by default."""

    validate_record(baseline)
    validate_record(candidate)
    if baseline["test_surface"] != candidate["test_surface"]:
        raise PerfRecordError("cross-surface timing comparison is forbidden")
    identity = ("build_profile", "firmware_sha256")
    if not allow_ab and any(baseline[name] != candidate[name] for name in identity):
        raise PerfRecordError("profile or firmware SHA differs; request an explicit A/B comparison")
    timings: dict[str, Any] = {}
    for name in TIMING_FIELDS:
        left = baseline[name]
        right = candidate[name]
        if left == NOT_APPLICABLE or right == NOT_APPLICABLE:
            timings[name] = NOT_APPLICABLE
        else:
            timings[name] = {
                percentile: {"baseline": left[percentile], "candidate": right[percentile]}
                for percentile in PERCENTILES
            }
    return {"test_surface": baseline["test_surface"], "timings": timings, "ab_comparison": allow_ab}


def prepare_baseline_update(
    old: Mapping[str, Any], new: Mapping[str, Any], reason: str, *, allow_ab: bool = False
) -> dict[str, Any]:
    """Return an auditable old/new/reason receipt; invalid baselines cannot be replaced."""

    if not isinstance(reason, str) or not reason.strip():
        raise PerfRecordError("baseline update requires a non-empty reason")
    validate_record(old)
    validate_record(new)
    comparison = compare_records(old, new, allow_ab=allow_ab)
    return {
        "schema_version": 1,
        "old": copy.deepcopy(dict(old)),
        "new": copy.deepcopy(dict(new)),
        "reason": reason.strip(),
        "comparison": comparison,
    }


def _load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise PerfRecordError(f"{path} must contain a JSON object")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate_parser = commands.add_parser("validate")
    validate_parser.add_argument("record", type=Path)
    validate_parser.add_argument("--profile-evidence", type=Path)
    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("baseline", type=Path)
    compare_parser.add_argument("candidate", type=Path)
    compare_parser.add_argument("--ab", action="store_true", help="explicit A/B profile or firmware comparison")
    update_parser = commands.add_parser("baseline-update")
    update_parser.add_argument("old", type=Path)
    update_parser.add_argument("new", type=Path)
    update_parser.add_argument("--reason", required=True)
    update_parser.add_argument("--output", type=Path, required=True)
    update_parser.add_argument("--ab", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "validate":
            record = _load(args.record)
            if args.profile_evidence:
                record = attach_build_evidence(record, args.profile_evidence)
            validate_record(record)
            print("PERF_RECORD_VALID: PASS")
        elif args.command == "compare":
            result = compare_records(_load(args.baseline), _load(args.candidate), allow_ab=args.ab)
            print(json.dumps(result, sort_keys=True))
        else:
            receipt = prepare_baseline_update(
                _load(args.old), _load(args.new), args.reason, allow_ab=args.ab
            )
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            print(f"PERF_BASELINE_UPDATE: {args.output}")
    except (OSError, json.JSONDecodeError, PerfRecordError) as error:
        print(f"PERF_RECORD_ERROR: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
