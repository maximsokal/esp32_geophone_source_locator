#!/usr/bin/env python3
"""Analyze storage/SPI telemetry using a measured hardware baseline JSON file."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import importlib.util
import json
from pathlib import Path
import sys
from typing import Any


REQUIRED_LIMIT_KEYS = (
    "mutex_wait_us",
    "tft_quiesce_us",
    "tft_resume_us",
    "spi_end_elapsed_us",
    "recorder_window_us",
    "recorder_fwrite_us",
    "recorder_operation_us",
)


class BaselineError(ValueError):
    pass


@dataclass(frozen=True)
class Baseline:
    measured_at: str
    board: str
    sd_card: str
    sdspi_hz: int
    sample_rate_hz: int
    channels: int
    firmware_commit: str
    limits_us: dict[str, int]
    notes: str


def _require_nonempty_string(data: dict[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise BaselineError(f"baseline field {key!r} must be a non-empty string")
    return value.strip()


def _require_positive_int(data: dict[str, Any], key: str) -> int:
    value = data.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise BaselineError(f"baseline field {key!r} must be a positive integer")
    return value


def load_baseline(path: Path) -> Baseline:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BaselineError(f"cannot read baseline {path}: {exc}") from exc

    if not isinstance(raw, dict):
        raise BaselineError("baseline root must be a JSON object")
    if raw.get("schema_version") != 1:
        raise BaselineError("baseline schema_version must be 1")

    limits_raw = raw.get("limits_us")
    if not isinstance(limits_raw, dict):
        raise BaselineError("baseline limits_us must be an object")

    limits: dict[str, int] = {}
    for key in REQUIRED_LIMIT_KEYS:
        value = limits_raw.get(key)
        if not isinstance(value, int) or isinstance(value, bool):
            raise BaselineError(f"baseline limit {key!r} must be an integer")
        if value < 0 or value > 60_000_000:
            raise BaselineError(
                f"baseline limit {key!r} must be in range 0..60000000"
            )
        limits[key] = value

    unexpected = sorted(set(limits_raw) - set(REQUIRED_LIMIT_KEYS))
    if unexpected:
        raise BaselineError(
            "baseline limits_us contains unknown keys: " + ", ".join(unexpected)
        )

    channels = _require_positive_int(raw, "channels")
    if channels > 8:
        raise BaselineError("baseline field 'channels' must be in range 1..8")

    notes = raw.get("notes", "")
    if not isinstance(notes, str):
        raise BaselineError("baseline field 'notes' must be a string when present")

    return Baseline(
        measured_at=_require_nonempty_string(raw, "measured_at"),
        board=_require_nonempty_string(raw, "board"),
        sd_card=_require_nonempty_string(raw, "sd_card"),
        sdspi_hz=_require_positive_int(raw, "sdspi_hz"),
        sample_rate_hz=_require_positive_int(raw, "sample_rate_hz"),
        channels=channels,
        firmware_commit=_require_nonempty_string(raw, "firmware_commit"),
        limits_us=limits,
        notes=notes.strip(),
    )


def _load_storage_analyzer(repo_root: Path):
    path = repo_root / "tools" / "analyze_storage_spi.py"
    spec = importlib.util.spec_from_file_location("analyze_storage_spi", path)
    if spec is None or spec.loader is None:
        raise BaselineError(f"cannot load analyzer module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="saved UART log")
    parser.add_argument("baseline", type=Path, help="measured baseline JSON")
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        baseline = load_baseline(args.baseline)
        content = args.log.read_text(encoding="utf-8", errors="replace")
        analyzer = _load_storage_analyzer(Path(__file__).resolve().parents[1])
    except (BaselineError, OSError) as exc:
        print(f"STORAGE_BASELINE_ANALYSIS: FAIL: {exc}", file=sys.stderr)
        return 1

    limits = baseline.limits_us
    result = analyzer.analyze(
        content,
        max_mutex_wait_us=limits["mutex_wait_us"],
        max_tft_quiesce_us=limits["tft_quiesce_us"],
        max_tft_resume_us=limits["tft_resume_us"],
        max_spi_end_elapsed_us=limits["spi_end_elapsed_us"],
        max_recorder_window_us=limits["recorder_window_us"],
        max_recorder_fwrite_us=limits["recorder_fwrite_us"],
        max_recorder_operation_us=limits["recorder_operation_us"],
        minimum_acquisition_diagnostic_channels=baseline.channels,
    )

    if args.json:
        print(
            json.dumps(
                {"baseline": asdict(baseline), "analysis": asdict(result)},
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print("===== Measured storage/SPI baseline =====")
        print(f"measured_at={baseline.measured_at}")
        print(f"board={baseline.board}")
        print(f"sd_card={baseline.sd_card}")
        print(f"sdspi_hz={baseline.sdspi_hz}")
        print(f"sample_rate_hz={baseline.sample_rate_hz}")
        print(f"channels={baseline.channels}")
        print(f"firmware_commit={baseline.firmware_commit}")
        if baseline.notes:
            print(f"notes={baseline.notes}")
        analyzer.print_report(result)

    if result.status == "PASS":
        return 0
    if result.status == "BLOCKED":
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
