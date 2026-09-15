#!/usr/bin/env python3
"""Analyze sustained recorder throughput and end-to-end SD integrity evidence.

The analyzer is deliberately offline: it never opens serial ports, flashes a
target, or controls hardware. A PASS is meaningful only when its inputs were
captured by the live HIL runner on the physical ESP32-S3 + SD-card assembly.
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

from ad7606_health import classify_evidence, uart_transport_violations


RECORDER_WRITE_RE = re.compile(
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
RECORDER_START_RE = re.compile(
    r"Recording\s+(?P<object>[^/\s]+)/(?P<session>\S+)\s+at\s+"
    r"(?P<rate>\d+)\s+Hz.*?PSRAM\s+cache=(?P<capacity>\d+)\s+frames",
    re.I,
)
STOP_RE = re.compile(
    r"STOP_COMPLETE\s+stage=(?P<stage>[a-z_]+).*?"
    r"session=(?P<session>\S+)\s+samples=(?P<samples>\d+)\s+"
    r"drops=(?P<drops>\d+)\s+err=(?P<err>[A-Z0-9_]+)",
    re.I,
)
READ_PATH_RE = re.compile(
    r"WAVEFORM_READ_PATH\s+source=(?P<source>[a-z0-9_]+)\s+"
    r"session=(?P<session>\S+)\s+start=(?P<start>\d+)\s+"
    r"frames=(?P<frames>\d+)\s+points=(?P<points>\d+)",
    re.I,
)
FAILURE_RE = re.compile(
    r"(?:Guru Meditation Error|panic(?:'ed|ed)?|task watchdog|brownout|"
    r"heap corruption|abort\(\)|assert failed|AD7606[_ ]?ACQ:.*Read failed|"
    r"Seismic buffer push failed)",
    re.I,
)


@dataclass(frozen=True)
class RecorderWrite:
    requested: int
    written: int
    windows: int
    operation_us: int
    result: int


@dataclass(frozen=True)
class Analysis:
    status: str
    missing: tuple[str, ...]
    violations: tuple[str, ...]
    session_id: str | None
    cache_capacity_frames: int | None
    write_samples: int
    written_frames: int
    required_stream_bps: float
    required_margin_bps: float
    aggregate_write_bps: float | None
    recording_ms: int | None
    samples_written: int | None
    integrity_overlap_frames: int
    raw_sd_read_confirmed: bool
    transport_health_status: str
    transport_health_sample_count: int
    signal_quality_states: tuple[str, ...]
    uart_transport_fault_count: int


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain one JSON object")
    return value


def _channel_map(channels: Any, value_key: str) -> dict[int, float]:
    result: dict[int, float] = {}
    if not isinstance(channels, list):
        return result
    for channel in channels:
        if not isinstance(channel, dict):
            continue
        try:
            channel_id = int(channel["id"])
            value = float(channel[value_key])
        except (KeyError, TypeError, ValueError):
            continue
        result[channel_id] = value
    return result


def _live_frames(payload: dict[str, Any]) -> dict[int, dict[int, float]]:
    timestamps = payload.get("timestamps_ms")
    channels = payload.get("channels")
    if not isinstance(timestamps, list) or not isinstance(channels, list):
        return {}

    channel_series: dict[int, list[Any]] = {}
    for channel in channels:
        if not isinstance(channel, dict):
            continue
        samples = channel.get("samples_uv")
        try:
            channel_id = int(channel.get("id"))
        except (TypeError, ValueError):
            continue
        if isinstance(samples, list):
            channel_series[channel_id] = samples

    frames: dict[int, dict[int, float]] = {}
    for index, timestamp in enumerate(timestamps):
        try:
            timestamp_ms = int(timestamp)
        except (TypeError, ValueError):
            continue
        values: dict[int, float] = {}
        valid = True
        for channel_id in range(1, 5):
            series = channel_series.get(channel_id)
            if series is None or index >= len(series):
                valid = False
                break
            try:
                values[channel_id] = float(series[index])
            except (TypeError, ValueError):
                valid = False
                break
        if valid:
            frames[timestamp_ms] = values
    return frames


def _saved_frames(
    payload: dict[str, Any],
    tolerance_uv: float,
) -> tuple[dict[int, dict[int, float]], list[str]]:
    points = payload.get("points")
    if not isinstance(points, list):
        return {}, ["saved payload has no points array"]

    frames: dict[int, dict[int, float]] = {}
    violations: list[str] = []
    for index, point in enumerate(points):
        if not isinstance(point, dict):
            violations.append(f"saved point {index} is not an object")
            continue
        try:
            timestamp_us = int(point["timestamp_us"])
        except (KeyError, TypeError, ValueError):
            violations.append(f"saved point {index} has invalid timestamp_us")
            continue

        mins = _channel_map(point.get("channels"), "min_uv")
        maxs = _channel_map(point.get("channels"), "max_uv")
        values: dict[int, float] = {}
        for channel_id in range(1, 5):
            if channel_id not in mins or channel_id not in maxs:
                violations.append(
                    f"saved point {index} missing channel {channel_id} min/max"
                )
                continue
            if abs(mins[channel_id] - maxs[channel_id]) > tolerance_uv:
                violations.append(
                    f"saved point {index} channel {channel_id} is reduced envelope, not one exact frame"
                )
                continue
            values[channel_id] = (mins[channel_id] + maxs[channel_id]) / 2.0
        if len(values) == 4:
            frames[timestamp_us // 1000] = values
    return frames, violations


def analyze(
    log_content: str,
    live_payload: dict[str, Any],
    saved_payload: dict[str, Any],
    *,
    sample_rate_hz: int,
    frame_size_bytes: int,
    safety_factor: float,
    minimum_write_samples: int,
    minimum_overlap_frames: int,
    integrity_tolerance_uv: float,
    health_evidence: dict[str, Any] | None = None,
) -> Analysis:
    missing: list[str] = []
    violations: list[str] = []

    starts = list(RECORDER_START_RE.finditer(log_content))
    start = starts[-1] if starts else None
    session_id = start.group("session") if start else None
    cache_capacity = int(start.group("capacity")) if start else None
    if start is None:
        missing.append("recorder start marker with PSRAM cache capacity")
    elif int(start.group("rate")) != sample_rate_hz:
        violations.append(
            f"recorder start rate={start.group('rate')}Hz expected={sample_rate_hz}Hz"
        )

    writes = [
        RecorderWrite(
            requested=int(match.group("requested")),
            written=int(match.group("written")),
            windows=int(match.group("windows")),
            operation_us=int(match.group("operation")),
            result=int(match.group("result")),
        )
        for match in RECORDER_WRITE_RE.finditer(log_content)
    ]
    if len(writes) < minimum_write_samples:
        missing.append(
            f"RECORDER_IO_TIMING fwrite samples ({len(writes)}/{minimum_write_samples})"
        )

    valid_write_us = 0
    written_frames = 0
    for sample in writes:
        if sample.result != 0:
            violations.append(f"recorder fwrite result={sample.result}")
        if sample.written != sample.requested:
            violations.append(
                f"recorder short write requested={sample.requested} written={sample.written}"
            )
        if sample.windows <= 0:
            violations.append("recorder fwrite reported zero storage windows")
        if sample.operation_us <= 0:
            violations.append("recorder fwrite reported non-positive operation_us")
        else:
            valid_write_us += sample.operation_us
            written_frames += sample.written

    required_stream_bps = float(sample_rate_hz * frame_size_bytes)
    required_margin_bps = required_stream_bps * safety_factor
    aggregate_write_bps: float | None = None
    if valid_write_us > 0:
        aggregate_write_bps = (
            float(written_frames * frame_size_bytes) * 1_000_000.0 / valid_write_us
        )
        if aggregate_write_bps < required_margin_bps:
            violations.append(
                "aggregate recorder write throughput "
                f"{aggregate_write_bps:.1f} B/s below {safety_factor:.2f}x margin "
                f"{required_margin_bps:.1f} B/s"
            )

    stops = list(STOP_RE.finditer(log_content))
    stop = stops[-1] if stops else None
    if stop is None:
        missing.append("SEISMIC_RECORDER STOP_COMPLETE")
    else:
        if stop.group("stage").lower() != "complete":
            violations.append(f"recorder stop stage={stop.group('stage')} expected=complete")
        if int(stop.group("drops")) != 0:
            violations.append(f"recorder dropped_frames={stop.group('drops')} expected=0")
        if stop.group("err").upper() != "ESP_OK":
            violations.append(f"recorder stop err={stop.group('err')} expected=ESP_OK")
        if session_id is not None and stop.group("session") != session_id:
            violations.append(
                f"stop session={stop.group('session')} does not match start session={session_id}"
            )

    failure_count = len(FAILURE_RE.findall(log_content))
    if failure_count:
        violations.append(f"target failure markers={failure_count} expected=0")

    health = classify_evidence(health_evidence)
    missing.extend(f"transport health: {item}" for item in health.missing)
    violations.extend(f"transport health: {item}" for item in health.violations)
    uart_faults = uart_transport_violations(log_content)
    violations.extend(uart_faults)

    recording_ms: int | None = None
    samples_written: int | None = None
    try:
        recording_ms = int(saved_payload["recording_ms"])
        samples_written = int(saved_payload["samples_written"])
    except (KeyError, TypeError, ValueError):
        missing.append("saved recording_ms/samples_written")

    saved_session = str(saved_payload.get("session_id", ""))
    if session_id is not None and saved_session != session_id:
        violations.append(
            f"saved session={saved_session or 'missing'} does not match {session_id}"
        )
    if saved_payload.get("complete") is not True:
        violations.append("saved session complete is not true")
    try:
        if int(saved_payload.get("dropped_frames", -1)) != 0:
            violations.append(
                f"saved dropped_frames={saved_payload.get('dropped_frames')} expected=0"
            )
        if int(saved_payload.get("sample_rate_hz", -1)) != sample_rate_hz:
            violations.append(
                f"saved sample_rate_hz={saved_payload.get('sample_rate_hz')} expected={sample_rate_hz}"
            )
        if int(saved_payload.get("channel_count", -1)) != 4:
            violations.append(
                f"saved channel_count={saved_payload.get('channel_count')} expected=4"
            )
    except (TypeError, ValueError):
        violations.append("saved numeric metadata is malformed")

    try:
        start_frame = int(saved_payload.get("start_frame", -1))
        frame_count = int(saved_payload.get("frame_count", -1))
        point_count = int(saved_payload.get("count", -1))
        source_frame_count = int(saved_payload.get("source_frame_count", -1))
    except (TypeError, ValueError):
        start_frame = frame_count = point_count = source_frame_count = -1
        violations.append("saved range metadata is malformed")

    if start_frame != 0:
        violations.append(f"saved integrity range start_frame={start_frame} expected=0")
    if frame_count <= 0 or point_count != frame_count:
        violations.append(
            f"saved integrity range must be one point per frame; frames={frame_count} points={point_count}"
        )

    if cache_capacity is not None and frame_count > 0:
        required_eviction_frames = cache_capacity + frame_count
        if source_frame_count <= required_eviction_frames:
            violations.append(
                "early integrity range not proven evicted from PSRAM cache: "
                f"source_frames={source_frame_count} must exceed {required_eviction_frames}"
            )

    raw_sd_read_confirmed = False
    if session_id is not None and frame_count > 0:
        for match in READ_PATH_RE.finditer(log_content):
            if (
                match.group("session") == session_id
                and match.group("source").lower() == "raw"
                and int(match.group("start")) == 0
                and int(match.group("frames")) == frame_count
                and int(match.group("points")) == point_count
            ):
                raw_sd_read_confirmed = True
                break
    if not raw_sd_read_confirmed:
        missing.append("WAVEFORM_READ_PATH source=raw for the exact integrity range")

    live_frames = _live_frames(live_payload)
    if not live_frames:
        missing.append("usable live /api/seismic reference frames")

    saved_frames, saved_frame_violations = _saved_frames(
        saved_payload,
        integrity_tolerance_uv,
    )
    violations.extend(saved_frame_violations)
    if not saved_frames:
        missing.append("usable exact saved waveform frames")

    overlap = sorted(set(live_frames).intersection(saved_frames))
    if len(overlap) < minimum_overlap_frames:
        missing.append(
            f"live/saved overlapping frames ({len(overlap)}/{minimum_overlap_frames})"
        )
    else:
        for timestamp_ms in overlap:
            live_values = live_frames[timestamp_ms]
            saved_values = saved_frames[timestamp_ms]
            for channel_id in range(1, 5):
                delta = abs(live_values[channel_id] - saved_values[channel_id])
                if delta > integrity_tolerance_uv:
                    violations.append(
                        "integrity mismatch "
                        f"t={timestamp_ms}ms ch={channel_id} "
                        f"live={live_values[channel_id]:.3f} "
                        f"saved={saved_values[channel_id]:.3f} "
                        f"delta={delta:.6f}uV"
                    )
                    if len(violations) >= 25:
                        break
            if len(violations) >= 25:
                break

    status = "FAIL" if violations else ("BLOCKED" if missing else "PASS")
    return Analysis(
        status=status,
        missing=tuple(missing),
        violations=tuple(violations),
        session_id=session_id,
        cache_capacity_frames=cache_capacity,
        write_samples=len(writes),
        written_frames=written_frames,
        required_stream_bps=required_stream_bps,
        required_margin_bps=required_margin_bps,
        aggregate_write_bps=aggregate_write_bps,
        recording_ms=recording_ms,
        samples_written=samples_written,
        integrity_overlap_frames=len(overlap),
        raw_sd_read_confirmed=raw_sd_read_confirmed,
        transport_health_status=health.status,
        transport_health_sample_count=len(health.samples),
        signal_quality_states=health.signal_quality_states,
        uart_transport_fault_count=len(uart_faults),
    )


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("live_json", type=Path)
    parser.add_argument("saved_json", type=Path)
    parser.add_argument("--health-evidence", type=Path)
    parser.add_argument("--sample-rate-hz", type=_positive_int, default=500)
    parser.add_argument("--frame-size-bytes", type=_positive_int, default=24)
    parser.add_argument("--safety-factor", type=_positive_float, default=4.0)
    parser.add_argument("--minimum-write-samples", type=_positive_int, default=30)
    parser.add_argument("--minimum-overlap-frames", type=_positive_int, default=64)
    parser.add_argument("--integrity-tolerance-uv", type=_positive_float, default=0.0015)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def print_report(result: Analysis) -> None:
    print(f"RECORDER_SUSTAINED_ANALYSIS: {result.status}")
    print(
        "session={0} cache_capacity_frames={1} write_samples={2} written_frames={3}".format(
            result.session_id or "missing",
            result.cache_capacity_frames if result.cache_capacity_frames is not None else "missing",
            result.write_samples,
            result.written_frames,
        )
    )
    throughput = (
        "missing"
        if result.aggregate_write_bps is None
        else f"{result.aggregate_write_bps:.1f}"
    )
    print(
        f"required_stream_bps={result.required_stream_bps:.1f} "
        f"required_margin_bps={result.required_margin_bps:.1f} "
        f"aggregate_write_bps={throughput}"
    )
    print(
        f"recording_ms={result.recording_ms} samples_written={result.samples_written} "
        f"integrity_overlap_frames={result.integrity_overlap_frames} "
        f"raw_sd_read_confirmed={str(result.raw_sd_read_confirmed).lower()}"
    )
    print(
        f"transport_health={result.transport_health_status} "
        f"health_samples={result.transport_health_sample_count} "
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


def main() -> int:
    args = parse_args()
    try:
        log_content = args.log.read_text(encoding="utf-8", errors="replace")
        live_payload = _load_json(args.live_json)
        saved_payload = _load_json(args.saved_json)
        health_evidence = (
            _load_json(args.health_evidence)
            if args.health_evidence is not None
            else None
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"RECORDER_SUSTAINED_ANALYSIS: FAIL: {exc}", file=sys.stderr)
        return 1

    result = analyze(
        log_content,
        live_payload,
        saved_payload,
        sample_rate_hz=args.sample_rate_hz,
        frame_size_bytes=args.frame_size_bytes,
        safety_factor=args.safety_factor,
        minimum_write_samples=args.minimum_write_samples,
        minimum_overlap_frames=args.minimum_overlap_frames,
        integrity_tolerance_uv=args.integrity_tolerance_uv,
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
