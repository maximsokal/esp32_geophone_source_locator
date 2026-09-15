#!/usr/bin/env python3
"""Pure helpers for classifying AD7606 transport health evidence.

The authoritative current state is the firmware ``/api/status`` acquisition
object.  Signal-domain quality is reported separately and never converted into
a transport/SPI/DMA failure by this module.
"""

from __future__ import annotations

from dataclasses import dataclass
import re
from typing import Any, Iterable


MINIMUM_QUALITY_WINDOW_FRAMES = 500
GOOD_STREAM_STATES = frozenset({"NORMAL", "QUIET"})
BAD_TRANSPORT_QUALITIES = frozenset({"DEGRADED", "FAULT"})
BAD_STREAM_STATES = frozenset({"SUSPICIOUS", "STUCK"})


@dataclass(frozen=True)
class HealthSample:
    status: str
    phase: str
    mature: bool
    transport_quality: str
    stream_state: str
    suspicious: bool | None
    stream_stuck: bool | None
    consecutive_failures: int | None
    quality_window_frames: int | None
    signal_quality: str
    reasons: tuple[str, ...]


@dataclass(frozen=True)
class HealthEvidence:
    status: str
    samples: tuple[HealthSample, ...]
    signal_quality_states: tuple[str, ...]
    missing: tuple[str, ...]
    violations: tuple[str, ...]


SERIAL_DIAG_RE = re.compile(
    r"AD7606_SERIAL_DIAG:.*?"
    r"transport_quality=(?P<quality>[A-Z_]+).*?"
    r"signal_quality=(?P<signal>[A-Z_]+).*?"
    r"stream_state=(?P<stream>[A-Z_]+).*?"
    r"stream_stuck=(?P<stuck>yes|no).*?"
    r"window=(?P<window>\d+).*?"
    r"serial_suspicious=(?P<suspicious>yes|no)",
    re.I,
)


def _upper(value: Any) -> str:
    return str(value).strip().upper() if value is not None else "UNKNOWN"


def _optional_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "yes", "1"}:
            return True
        if lowered in {"false", "no", "0"}:
            return False
    if isinstance(value, int) and value in {0, 1}:
        return bool(value)
    return None


def _optional_int(value: Any) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _acquisition(payload: Any) -> dict[str, Any] | None:
    if not isinstance(payload, dict):
        return None
    acquisition = payload.get("acquisition")
    if isinstance(acquisition, dict):
        return acquisition
    if "serial" in payload or "error_counters_available" in payload:
        return payload
    return None


def classify_status(
    payload: Any,
    *,
    phase: str = "unknown",
    minimum_window_frames: int = MINIMUM_QUALITY_WINDOW_FRAMES,
) -> HealthSample:
    acquisition = _acquisition(payload)
    if acquisition is None:
        return HealthSample(
            status="BLOCKED",
            phase=phase,
            mature=False,
            transport_quality="UNKNOWN",
            stream_state="UNKNOWN",
            suspicious=None,
            stream_stuck=None,
            consecutive_failures=None,
            quality_window_frames=None,
            signal_quality="UNKNOWN",
            reasons=("missing acquisition health object",),
        )

    serial = acquisition.get("serial")
    if not isinstance(serial, dict):
        serial = {}
    signal = acquisition.get("signal")
    if not isinstance(signal, dict):
        signal = {}

    counters_available = _optional_bool(acquisition.get("error_counters_available"))
    quality_window_frames = _optional_int(serial.get("quality_window_frames"))
    transport_quality = _upper(serial.get("quality"))
    stream_state = _upper(serial.get("stream_state"))
    suspicious = _optional_bool(serial.get("suspicious"))
    stream_stuck = _optional_bool(serial.get("stream_stuck"))
    consecutive_failures = _optional_int(acquisition.get("consecutive_failures"))
    signal_quality = _upper(signal.get("quality"))

    blocked: list[str] = []
    violations: list[str] = []

    if counters_available is not True:
        blocked.append("error_counters_available is not true")

    mature = (
        counters_available is True
        and quality_window_frames is not None
        and quality_window_frames >= minimum_window_frames
    )
    if quality_window_frames is None:
        blocked.append("serial quality_window_frames is missing")
    elif quality_window_frames < minimum_window_frames:
        blocked.append(
            f"serial quality window immature ({quality_window_frames}/{minimum_window_frames})"
        )

    if mature:
        if transport_quality in BAD_TRANSPORT_QUALITIES:
            violations.append(f"transport quality={transport_quality}")
        elif transport_quality != "HEALTHY":
            blocked.append(f"transport quality={transport_quality}")

        if stream_state in BAD_STREAM_STATES:
            violations.append(f"stream state={stream_state}")
        elif stream_state not in GOOD_STREAM_STATES:
            blocked.append(f"stream state={stream_state}")

        if suspicious is True:
            violations.append("serial suspicious=true")
        elif suspicious is not False:
            blocked.append("serial suspicious flag is unavailable")

        if stream_stuck is True:
            violations.append("serial stream_stuck=true")
        elif stream_stuck is not False:
            blocked.append("serial stream_stuck flag is unavailable")

        if consecutive_failures is None:
            blocked.append("consecutive_failures is unavailable")
        elif consecutive_failures != 0:
            violations.append(
                f"consecutive_failures={consecutive_failures} expected=0"
            )

    status = "FAIL" if violations else ("BLOCKED" if blocked else "PASS")
    return HealthSample(
        status=status,
        phase=phase,
        mature=mature,
        transport_quality=transport_quality,
        stream_state=stream_state,
        suspicious=suspicious,
        stream_stuck=stream_stuck,
        consecutive_failures=consecutive_failures,
        quality_window_frames=quality_window_frames,
        signal_quality=signal_quality,
        reasons=tuple(violations or blocked),
    )


def _iter_evidence_samples(evidence: Any) -> Iterable[tuple[str, Any]]:
    if not isinstance(evidence, dict):
        return ()
    samples = evidence.get("samples")
    if isinstance(samples, list):
        result: list[tuple[str, Any]] = []
        for index, item in enumerate(samples):
            if isinstance(item, dict) and "status" in item:
                result.append((str(item.get("phase", f"sample-{index}")), item["status"]))
            else:
                result.append((f"sample-{index}", item))
        return tuple(result)
    return ((str(evidence.get("phase", "sample-0")), evidence.get("status", evidence)),)


def classify_evidence(
    evidence: Any,
    *,
    minimum_window_frames: int = MINIMUM_QUALITY_WINDOW_FRAMES,
) -> HealthEvidence:
    samples = tuple(
        classify_status(
            payload,
            phase=phase,
            minimum_window_frames=minimum_window_frames,
        )
        for phase, payload in _iter_evidence_samples(evidence)
    )

    missing: list[str] = []
    violations: list[str] = []
    if not samples:
        missing.append("AD7606 transport health evidence")

    for sample in samples:
        prefix = f"{sample.phase}: "
        if sample.status == "FAIL":
            violations.extend(prefix + reason for reason in sample.reasons)
        elif sample.status == "BLOCKED":
            missing.extend(prefix + reason for reason in sample.reasons)

    signal_states = tuple(
        sorted({sample.signal_quality for sample in samples if sample.signal_quality != "UNKNOWN"})
    )
    status = "FAIL" if violations else ("BLOCKED" if missing else "PASS")
    return HealthEvidence(
        status=status,
        samples=samples,
        signal_quality_states=signal_states,
        missing=tuple(missing),
        violations=tuple(violations),
    )


def uart_transport_violations(
    content: str,
    *,
    minimum_window_frames: int = MINIMUM_QUALITY_WINDOW_FRAMES,
) -> tuple[str, ...]:
    """Return sticky mature transport faults observed in event-driven UART diagnostics."""
    violations: list[str] = []
    for match in SERIAL_DIAG_RE.finditer(content):
        window = int(match.group("window"))
        if window < minimum_window_frames:
            continue
        quality = match.group("quality").upper()
        stream = match.group("stream").upper()
        stuck = match.group("stuck").lower() == "yes"
        suspicious = match.group("suspicious").lower() == "yes"
        reasons: list[str] = []
        if quality in BAD_TRANSPORT_QUALITIES:
            reasons.append(f"transport quality={quality}")
        if stream in BAD_STREAM_STATES:
            reasons.append(f"stream state={stream}")
        if suspicious:
            reasons.append("serial suspicious=true")
        if stuck:
            reasons.append("serial stream_stuck=true")
        if reasons:
            violations.append(
                f"UART mature AD7606 transport fault window={window}: " + ", ".join(reasons)
            )
    return tuple(violations)
