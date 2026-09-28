#!/usr/bin/env python3
"""Analyze already-captured storage/SPI/recorder UART telemetry offline.

The analyzer never opens serial ports, flashes firmware, resets hardware, or
controls power. Latency thresholds are required explicitly because acceptable
limits must come from a measured real-board baseline, not parser defaults.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import sys
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from ad7606_health import classify_evidence, sample_rate_hz_from_evidence, uart_transport_violations


@dataclass(frozen=True)
class StorageBegin:
    mutex_wait_us: int
    tft_quiesce_us: int
    result: int


@dataclass(frozen=True)
class StorageEnd:
    tft_resume_us: int
    elapsed_us: int


@dataclass(frozen=True)
class RecorderFwrite:
    requested: int
    written: int
    windows: int
    max_window_us: int
    fwrite_us: int
    operation_us: int
    result: int


@dataclass(frozen=True)
class RecorderStop:
    stage: str
    dropped_frames: int
    error_name: str


@dataclass(frozen=True)
class Analysis:
    status: str
    missing: tuple[str, ...]
    violations: tuple[str, ...]
    storage_begin_count: int
    storage_end_count: int
    recorder_count: int
    recorder_stop_count: int
    acquisition_diagnostic_channels: tuple[int, ...]
    acquisition_failure_count: int
    failure_marker_count: int
    max_dropped_frames: int | None
    max_mutex_wait_us: int | None
    max_tft_quiesce_us: int | None
    max_tft_resume_us: int | None
    max_spi_end_elapsed_us: int | None
    max_recorder_window_us: int | None
    max_recorder_fwrite_us: int | None
    max_recorder_operation_us: int | None
    transport_health_status: str
    transport_health_sample_count: int
    signal_quality_states: tuple[str, ...]
    uart_transport_fault_count: int


BEGIN_RE = re.compile(
    r"STORAGE_SPI_TIMING\s+stage=begin\s+"
    r"mutex_wait_us=(?P<mutex>\d+)\s+"
    r"tft_quiesce_us=(?P<quiesce>\d+)\s+"
    r"result=(?P<result>-?\d+)",
    re.I,
)
END_RE = re.compile(
    r"STORAGE_SPI_TIMING\s+stage=end\s+"
    r"tft_resume_us=(?P<resume>\d+)\s+"
    r"elapsed_us=(?P<elapsed>\d+)",
    re.I,
)
RECORDER_RE = re.compile(
    r"RECORDER_IO_TIMING\s+op=fwrite\s+"
    r"requested=(?P<requested>\d+)\s+"
    r"written=(?P<written>\d+)\s+"
    r"windows=(?P<windows>\d+)\s+"
    r"max_window_us=(?P<window>\d+)\s+"
    r"fwrite_us=(?P<fwrite>\d+)\s+"
    r"operation_us=(?P<operation>\d+)\s+"
    r"result=(?P<result>-?\d+)",
    re.I,
)
STOP_RE = re.compile(
    r"STOP_COMPLETE\s+stage=(?P<stage>[a-z_]+).*?"
    r"drops=(?P<drops>\d+)\s+err=(?P<err>[A-Z0-9_]+)",
    re.I,
)
DIAG_RE = re.compile(
    r"AD7606_DIAG:\s*CH(?P<channel>[1-8])\s+n=(?P<samples>[1-9]\d*)",
    re.I,
)
ACQUISITION_FAILURE_RE = re.compile(
    r"(?:AD7606[_ ]?ACQ:.*Read failed|Seismic buffer push failed|"
    r"Core 1 acquisition initialization failed)",
    re.I,
)
FAILURE_LINE_RE = re.compile(
    r"^.*(?:Guru Meditation Error|panic(?:'ed|ed)?|task watchdog|brownout|"
    r"heap corruption|abort\(\)|assert failed).*$",
    re.I | re.M,
)


def _maximum(values: list[int]) -> int | None:
    return max(values) if values else None


def analyze(
    content: str,
    *,
    max_mutex_wait_us: int,
    max_tft_quiesce_us: int,
    max_tft_resume_us: int,
    max_spi_end_elapsed_us: int,
    max_recorder_window_us: int,
    max_recorder_fwrite_us: int,
    max_recorder_operation_us: int,
    minimum_storage_samples: int = 1,
    minimum_recorder_samples: int = 1,
    minimum_recorder_stop_samples: int = 1,
    minimum_acquisition_diagnostic_channels: int = 8,
    health_evidence: dict[str, Any] | None = None,
) -> Analysis:
    begins = [
        StorageBegin(
            mutex_wait_us=int(match.group("mutex")),
            tft_quiesce_us=int(match.group("quiesce")),
            result=int(match.group("result")),
        )
        for match in BEGIN_RE.finditer(content)
    ]
    ends = [
        StorageEnd(
            tft_resume_us=int(match.group("resume")),
            elapsed_us=int(match.group("elapsed")),
        )
        for match in END_RE.finditer(content)
    ]
    recorder = [
        RecorderFwrite(
            requested=int(match.group("requested")),
            written=int(match.group("written")),
            windows=int(match.group("windows")),
            max_window_us=int(match.group("window")),
            fwrite_us=int(match.group("fwrite")),
            operation_us=int(match.group("operation")),
            result=int(match.group("result")),
        )
        for match in RECORDER_RE.finditer(content)
    ]
    stops = [
        RecorderStop(
            stage=match.group("stage").lower(),
            dropped_frames=int(match.group("drops")),
            error_name=match.group("err").upper(),
        )
        for match in STOP_RE.finditer(content)
    ]
    diagnostic_channels = tuple(
        sorted({int(match.group("channel")) for match in DIAG_RE.finditer(content)})
    )
    acquisition_failure_count = len(ACQUISITION_FAILURE_RE.findall(content))
    failure_marker_count = len(FAILURE_LINE_RE.findall(content))

    missing: list[str] = []
    if len(begins) < minimum_storage_samples:
        missing.append(
            f"STORAGE_SPI_TIMING stage=begin ({len(begins)}/{minimum_storage_samples})"
        )
    if len(ends) < minimum_storage_samples:
        missing.append(
            f"STORAGE_SPI_TIMING stage=end ({len(ends)}/{minimum_storage_samples})"
        )
    if len(recorder) < minimum_recorder_samples:
        missing.append(
            f"RECORDER_IO_TIMING op=fwrite ({len(recorder)}/{minimum_recorder_samples})"
        )
    if len(stops) < minimum_recorder_stop_samples:
        missing.append(
            f"SEISMIC_RECORDER STOP_COMPLETE ({len(stops)}/{minimum_recorder_stop_samples})"
        )
    if len(diagnostic_channels) < minimum_acquisition_diagnostic_channels:
        missing.append(
            "AD7606_DIAG positive-sample channels "
            f"({len(diagnostic_channels)}/{minimum_acquisition_diagnostic_channels})"
        )

    violations: list[str] = []
    for sample in begins:
        if sample.result != 0:
            violations.append(f"storage begin result={sample.result}")

    for sample in recorder:
        if sample.result != 0:
            violations.append(f"recorder fwrite result={sample.result}")
        if sample.written != sample.requested:
            violations.append(
                "recorder short write "
                f"requested={sample.requested} written={sample.written}"
            )
        if sample.windows == 0:
            violations.append("recorder fwrite reported zero storage windows")

    for stop in stops:
        if stop.stage != "complete":
            violations.append(f"recorder stop stage={stop.stage} expected=complete")
        if stop.dropped_frames != 0:
            violations.append(
                f"recorder dropped_frames={stop.dropped_frames} expected=0"
            )
        if stop.error_name != "ESP_OK":
            violations.append(
                f"recorder stop err={stop.error_name} expected=ESP_OK"
            )

    if acquisition_failure_count:
        violations.append(
            f"acquisition failure markers={acquisition_failure_count} expected=0"
        )
    if failure_marker_count:
        violations.append(
            "panic/watchdog/brownout/assert marker observed "
            f"count={failure_marker_count}"
        )

    health = classify_evidence(health_evidence)
    missing.extend(f"transport health: {item}" for item in health.missing)
    violations.extend(f"transport health: {item}" for item in health.violations)
    uart_faults = uart_transport_violations(
        content,
        sample_rate_hz=sample_rate_hz_from_evidence(health_evidence),
    )
    violations.extend(uart_faults)

    max_mutex = _maximum([sample.mutex_wait_us for sample in begins])
    max_quiesce = _maximum([sample.tft_quiesce_us for sample in begins])
    max_resume = _maximum([sample.tft_resume_us for sample in ends])
    max_elapsed = _maximum([sample.elapsed_us for sample in ends])
    max_window = _maximum([sample.max_window_us for sample in recorder])
    max_fwrite = _maximum([sample.fwrite_us for sample in recorder])
    max_operation = _maximum([sample.operation_us for sample in recorder])
    max_drops = _maximum([sample.dropped_frames for sample in stops])

    checks = (
        ("mutex_wait_us", max_mutex, max_mutex_wait_us),
        ("tft_quiesce_us", max_quiesce, max_tft_quiesce_us),
        ("tft_resume_us", max_resume, max_tft_resume_us),
        ("spi_end_elapsed_us", max_elapsed, max_spi_end_elapsed_us),
        ("max_window_us", max_window, max_recorder_window_us),
        ("fwrite_us", max_fwrite, max_recorder_fwrite_us),
        ("recorder_operation_us", max_operation, max_recorder_operation_us),
    )
    for name, value, limit in checks:
        if value is not None and value > limit:
            violations.append(f"{name}={value}us exceeds limit={limit}us")

    status = "FAIL" if violations else ("BLOCKED" if missing else "PASS")
    return Analysis(
        status=status,
        missing=tuple(missing),
        violations=tuple(violations),
        storage_begin_count=len(begins),
        storage_end_count=len(ends),
        recorder_count=len(recorder),
        recorder_stop_count=len(stops),
        acquisition_diagnostic_channels=diagnostic_channels,
        acquisition_failure_count=acquisition_failure_count,
        failure_marker_count=failure_marker_count,
        max_dropped_frames=max_drops,
        max_mutex_wait_us=max_mutex,
        max_tft_quiesce_us=max_quiesce,
        max_tft_resume_us=max_resume,
        max_spi_end_elapsed_us=max_elapsed,
        max_recorder_window_us=max_window,
        max_recorder_fwrite_us=max_fwrite,
        max_recorder_operation_us=max_operation,
        transport_health_status=health.status,
        transport_health_sample_count=len(health.samples),
        signal_quality_states=health.signal_quality_states,
        uart_transport_fault_count=len(uart_faults),
    )


def print_report(result: Analysis) -> None:
    print(f"STORAGE_SPI_ANALYSIS: {result.status}")
    print(
        "samples "
        f"storage_begin={result.storage_begin_count} "
        f"storage_end={result.storage_end_count} "
        f"recorder_fwrite={result.recorder_count} "
        f"recorder_stop={result.recorder_stop_count}"
    )
    print(
        "acquisition_diag_channels="
        + (",".join(map(str, result.acquisition_diagnostic_channels)) or "none")
    )
    print(f"max_mutex_wait_us={result.max_mutex_wait_us}")
    print(f"max_tft_quiesce_us={result.max_tft_quiesce_us}")
    print(f"max_tft_resume_us={result.max_tft_resume_us}")
    print(f"max_spi_end_elapsed_us={result.max_spi_end_elapsed_us}")
    print(f"max_recorder_window_us={result.max_recorder_window_us}")
    print(f"max_recorder_fwrite_us={result.max_recorder_fwrite_us}")
    print(f"max_recorder_operation_us={result.max_recorder_operation_us}")
    print(f"max_dropped_frames={result.max_dropped_frames}")
    print(f"acquisition_failure_markers={result.acquisition_failure_count}")
    print(f"failure_markers={result.failure_marker_count}")
    print(
        f"transport_health={result.transport_health_status} "
        f"samples={result.transport_health_sample_count} "
        f"uart_faults={result.uart_transport_fault_count}"
    )
    print(
        "signal_quality_states="
        + (",".join(result.signal_quality_states) or "unknown")
    )
    for item in result.missing:
        print(f"MISSING: {item}")
    for item in result.violations:
        print(f"FAIL: {item}")


def _bounded_nonnegative(value: str) -> int:
    parsed = int(value)
    if parsed < 0 or parsed > 60_000_000:
        raise argparse.ArgumentTypeError("must be in range 0..60000000")
    return parsed


def _positive(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="saved UART log file")
    parser.add_argument("--health-evidence", type=Path)
    parser.add_argument("--max-mutex-wait-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-tft-quiesce-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-tft-resume-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-spi-end-elapsed-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-recorder-window-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-recorder-fwrite-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--max-recorder-operation-us", type=_bounded_nonnegative, required=True)
    parser.add_argument("--minimum-storage-samples", type=_positive, default=1)
    parser.add_argument("--minimum-recorder-samples", type=_positive, default=1)
    parser.add_argument("--minimum-recorder-stop-samples", type=_positive, default=1)
    parser.add_argument(
        "--minimum-acquisition-diagnostic-channels",
        type=_positive,
        default=8,
    )
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def _load_health_evidence(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    with path.open("r", encoding="utf-8-sig") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain one JSON object")
    return value


def main() -> int:
    args = parse_args()
    try:
        content = args.log.read_text(encoding="utf-8", errors="replace")
        health_evidence = _load_health_evidence(args.health_evidence)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"STORAGE_SPI_ANALYSIS: FAIL: {exc}", file=sys.stderr)
        return 1

    result = analyze(
        content,
        max_mutex_wait_us=args.max_mutex_wait_us,
        max_tft_quiesce_us=args.max_tft_quiesce_us,
        max_tft_resume_us=args.max_tft_resume_us,
        max_spi_end_elapsed_us=args.max_spi_end_elapsed_us,
        max_recorder_window_us=args.max_recorder_window_us,
        max_recorder_fwrite_us=args.max_recorder_fwrite_us,
        max_recorder_operation_us=args.max_recorder_operation_us,
        minimum_storage_samples=args.minimum_storage_samples,
        minimum_recorder_samples=args.minimum_recorder_samples,
        minimum_recorder_stop_samples=args.minimum_recorder_stop_samples,
        minimum_acquisition_diagnostic_channels=args.minimum_acquisition_diagnostic_channels,
        health_evidence=health_evidence,
    )

    if args.json:
        print(json.dumps(asdict(result), indent=2, sort_keys=True))
    else:
        print_report(result)

    if result.status == "PASS":
        return 0
    if result.status == "BLOCKED":
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
