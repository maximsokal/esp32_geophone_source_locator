#!/usr/bin/env python3
"""Analyze an already-captured ESP32 UART log without opening serial ports.

This is intentionally offline: it never flashes, resets, powers, or opens COM
ports. It classifies a saved log as PASS, BLOCKED (required evidence missing),
or FAIL (evidence present but an acceptance rule is violated).
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import sys
from typing import Iterable


EXPECTED_KEYS = {1: "LEFT", 2: "UP", 3: "DOWN", 4: "RIGHT", 5: "OK"}
KEY_NAME_TO_CODE = {name: code for code, name in EXPECTED_KEYS.items()}


@dataclass(frozen=True)
class RecoverySample:
    object_id: str
    elapsed_ms: int
    removed: int


@dataclass(frozen=True)
class DispatchSample:
    key: int
    age_ms: int


@dataclass(frozen=True)
class UiResultSample:
    key: int
    screen_before: int
    screen_after: int
    selected_before: int
    selected_after: int

    @property
    def changed(self) -> bool:
        return (
            self.screen_before != self.screen_after
            or self.selected_before != self.selected_after
        )


@dataclass(frozen=True)
class PrioritySample:
    current: int
    base: int
    inherited: int


@dataclass(frozen=True)
class Analysis:
    status: str
    missing: tuple[str, ...]
    violations: tuple[str, ...]
    startup_ms: int | None
    max_recovery_window_ms: int | None
    max_dispatch_age_ms: int | None
    raw_keys: tuple[int, ...]
    button_keys: tuple[int, ...]
    posted_keys: tuple[int, ...]
    dispatched_keys: tuple[int, ...]
    ui_result_keys: tuple[int, ...]
    ui_changed_keys: tuple[int, ...]
    stale_drop_count: int
    safe_shutdown_count: int
    failure_marker_count: int
    priority_sample_count: int
    inherited_priority_observed: bool


STARTUP_RE = re.compile(r"BOOT_TIMING:\s*startup_total=(?P<ms>\d+)\s*ms", re.I)
RECOVERY_RE = re.compile(
    r"OBJECT_RECOVERY_PROGRESS\s+object=(?P<object>OBJ_\d{4})\s+"
    r"index=\d+\s+total=\d+\s+removed=(?P<removed>[01])\s+"
    r"elapsed_ms=(?P<ms>\d+)",
    re.I,
)
RAW_RE = re.compile(
    r"KEYPAD_RAW_TRANSITION\s+raw=-?\d+\s+from=\S+\s+to=(?P<key>LEFT|UP|DOWN|RIGHT|OK|NONE)",
    re.I,
)
BUTTON_RE = re.compile(
    r"KEYPAD_BUTTON_PRESS\s+key=(?P<name>LEFT|UP|DOWN|RIGHT|OK)\s+value=(?P<key>\d+)",
    re.I,
)
POST_RE = re.compile(r"KEYPAD_EVENT_POST\s+key=(?P<key>-?\d+)", re.I)
DISPATCH_RE = re.compile(
    r"KEYPAD_EVENT_DISPATCH\s+key=(?P<key>-?\d+)\s+age_ms=(?P<age>\d+)",
    re.I,
)
UI_RESULT_RE = re.compile(
    r"UI_KEY_RESULT\s+key=(?P<key>-?\d+)\s+"
    r"screen_before=(?P<screen_before>-?\d+)\s+"
    r"screen_after=(?P<screen_after>-?\d+)\s+"
    r"selected_before=(?P<selected_before>\d+)\s+"
    r"selected_after=(?P<selected_after>\d+)",
    re.I,
)
STALE_RE = re.compile(r"Dropped stale keypad navigation\s+age_ms=\d+\s+total=\d+", re.I)
PRIORITY_RE = re.compile(
    r"RUNTIME_TASK\s+rank=\d+\s+name=web_status_cach(?:e)?\s+core=-?\d+\s+"
    r"priority_current=(?P<current>\d+)\s+priority_base=(?P<base>\d+)\s+"
    r"inherited=(?P<inherited>[01])",
    re.I,
)
FAILURE_LINE_RE = re.compile(
    r"^.*(?:Guru Meditation Error|panic'ed|Task watchdog got triggered|"
    r"Brownout detector was triggered|assert failed).*$",
    re.I | re.M,
)


def _unique_valid_keys(values: Iterable[int]) -> tuple[int, ...]:
    return tuple(sorted({value for value in values if value in EXPECTED_KEYS}))


def _missing_keys(label: str, seen: set[int]) -> list[str]:
    missing = sorted(set(EXPECTED_KEYS) - seen)
    return [f"{label} missing={','.join(map(str, missing))}"] if missing else []


def analyze(
    content: str,
    *,
    max_startup_ms: int = 5000,
    max_recovery_window_ms: int = 250,
    expected_web_cache_base_priority: int = 1,
) -> Analysis:
    missing: list[str] = []
    violations: list[str] = []

    startup_values = [int(m.group("ms")) for m in STARTUP_RE.finditer(content)]
    startup_ms = max(startup_values) if startup_values else None
    if startup_ms is None:
        missing.append("BOOT_TIMING startup_total")
    elif startup_ms > max_startup_ms:
        violations.append(
            f"startup_total={startup_ms}ms exceeds limit={max_startup_ms}ms"
        )

    recovery = [
        RecoverySample(m.group("object"), int(m.group("ms")), int(m.group("removed")))
        for m in RECOVERY_RE.finditer(content)
    ]
    if not recovery:
        missing.append("OBJECT_RECOVERY_PROGRESS")
    max_recovery = max((sample.elapsed_ms for sample in recovery), default=None)
    if max_recovery is not None and max_recovery > max_recovery_window_ms:
        violations.append(
            f"recovery_window={max_recovery}ms exceeds limit={max_recovery_window_ms}ms"
        )

    raw_keys: set[int] = set()
    for match in RAW_RE.finditer(content):
        name = match.group("key").upper()
        if name in KEY_NAME_TO_CODE:
            raw_keys.add(KEY_NAME_TO_CODE[name])

    button_keys: set[int] = set()
    for match in BUTTON_RE.finditer(content):
        key = int(match.group("key"))
        name = match.group("name").upper()
        expected = KEY_NAME_TO_CODE.get(name)
        if expected != key:
            violations.append(f"KEYPAD_BUTTON_PRESS inconsistent name={name} value={key}")
        button_keys.add(key)

    posted_keys = {int(m.group("key")) for m in POST_RE.finditer(content)}
    dispatch = [
        DispatchSample(int(m.group("key")), int(m.group("age")))
        for m in DISPATCH_RE.finditer(content)
    ]
    dispatched_keys = {sample.key for sample in dispatch}
    max_dispatch_age = max((sample.age_ms for sample in dispatch), default=None)

    ui_results = [
        UiResultSample(
            key=int(m.group("key")),
            screen_before=int(m.group("screen_before")),
            screen_after=int(m.group("screen_after")),
            selected_before=int(m.group("selected_before")),
            selected_after=int(m.group("selected_after")),
        )
        for m in UI_RESULT_RE.finditer(content)
    ]
    ui_result_keys = {sample.key for sample in ui_results}
    ui_changed_keys = {sample.key for sample in ui_results if sample.changed}

    missing.extend(_missing_keys("KEYPAD_RAW_TRANSITION", raw_keys))
    missing.extend(_missing_keys("KEYPAD_BUTTON_PRESS", button_keys))
    missing.extend(_missing_keys("KEYPAD_EVENT_POST", posted_keys))
    missing.extend(_missing_keys("KEYPAD_EVENT_DISPATCH", dispatched_keys))
    missing.extend(_missing_keys("UI_KEY_RESULT", ui_result_keys))

    for label, values in (
        ("KEYPAD_BUTTON_PRESS", button_keys),
        ("KEYPAD_EVENT_POST", posted_keys),
        ("KEYPAD_EVENT_DISPATCH", dispatched_keys),
        ("UI_KEY_RESULT", ui_result_keys),
    ):
        unexpected = sorted(values - set(EXPECTED_KEYS))
        if unexpected:
            violations.append(f"{label} unexpected key codes={','.join(map(str, unexpected))}")

    stale_count = len(STALE_RE.findall(content))
    if stale_count:
        violations.append(f"stale keypad drops observed count={stale_count}")

    safe_shutdown_count = len(re.findall(r"Safe shutdown started", content, re.I))
    if safe_shutdown_count:
        violations.append(f"unexpected safe shutdown observed count={safe_shutdown_count}")

    failure_marker_count = len(FAILURE_LINE_RE.findall(content))
    if failure_marker_count:
        violations.append(
            f"panic/watchdog/brownout/assert marker observed count={failure_marker_count}"
        )

    startup_pos = content.lower().find("boot_timing: startup_total=")
    recovery_start_pos = content.lower().find("object_recovery_background state=start")
    recovery_stop_pos = content.lower().find("object_recovery_background state=stop")
    cache_done_pos = content.lower().find("web_status_cache refresh=done")
    if recovery_start_pos < 0:
        missing.append("OBJECT_RECOVERY_BACKGROUND state=START")
    if recovery_stop_pos < 0:
        missing.append("OBJECT_RECOVERY_BACKGROUND state=STOP")
    if cache_done_pos < 0:
        missing.append("WEB_STATUS_CACHE refresh=done")
    if startup_pos >= 0 and recovery_start_pos >= 0 and recovery_start_pos < startup_pos:
        violations.append("background recovery started before startup_total")
    if recovery_stop_pos >= 0 and cache_done_pos >= 0 and cache_done_pos < recovery_stop_pos:
        violations.append("Web status cache completed before background recovery stopped")

    priorities = [
        PrioritySample(
            current=int(m.group("current")),
            base=int(m.group("base")),
            inherited=int(m.group("inherited")),
        )
        for m in PRIORITY_RE.finditer(content)
    ]
    inherited_observed = False
    for sample in priorities:
        if sample.base != expected_web_cache_base_priority:
            violations.append(
                f"web_status_cache priority_base={sample.base} "
                f"expected={expected_web_cache_base_priority}"
            )
        expected_inherited = int(sample.current > sample.base)
        if sample.inherited != expected_inherited:
            violations.append(
                "web_status_cache inherited="
                f"{sample.inherited} inconsistent with current={sample.current} base={sample.base}"
            )
        inherited_observed = inherited_observed or bool(sample.inherited)

    status = "FAIL" if violations else ("BLOCKED" if missing else "PASS")
    return Analysis(
        status=status,
        missing=tuple(missing),
        violations=tuple(violations),
        startup_ms=startup_ms,
        max_recovery_window_ms=max_recovery,
        max_dispatch_age_ms=max_dispatch_age,
        raw_keys=_unique_valid_keys(raw_keys),
        button_keys=_unique_valid_keys(button_keys),
        posted_keys=_unique_valid_keys(posted_keys),
        dispatched_keys=_unique_valid_keys(dispatched_keys),
        ui_result_keys=_unique_valid_keys(ui_result_keys),
        ui_changed_keys=_unique_valid_keys(ui_changed_keys),
        stale_drop_count=stale_count,
        safe_shutdown_count=safe_shutdown_count,
        failure_marker_count=failure_marker_count,
        priority_sample_count=len(priorities),
        inherited_priority_observed=inherited_observed,
    )


def _format_keys(values: tuple[int, ...]) -> str:
    if not values:
        return "none"
    return ",".join(f"{key}:{EXPECTED_KEYS[key]}" for key in values)


def print_report(result: Analysis) -> None:
    print(f"BOOT_RESPONSIVENESS_ANALYSIS: {result.status}")
    print(f"startup_ms={result.startup_ms}")
    print(f"max_recovery_window_ms={result.max_recovery_window_ms}")
    print(f"max_dispatch_age_ms={result.max_dispatch_age_ms}")
    print(f"raw_keys={_format_keys(result.raw_keys)}")
    print(f"button_keys={_format_keys(result.button_keys)}")
    print(f"posted_keys={_format_keys(result.posted_keys)}")
    print(f"dispatched_keys={_format_keys(result.dispatched_keys)}")
    print(f"ui_result_keys={_format_keys(result.ui_result_keys)}")
    print(f"ui_changed_keys={_format_keys(result.ui_changed_keys)}")
    print(
        "stale_drops="
        f"{result.stale_drop_count} safe_shutdowns={result.safe_shutdown_count} "
        f"failure_markers={result.failure_marker_count}"
    )
    print(
        "priority_samples="
        f"{result.priority_sample_count} inherited_observed="
        f"{int(result.inherited_priority_observed)}"
    )
    for item in result.missing:
        print(f"MISSING: {item}")
    for item in result.violations:
        print(f"FAIL: {item}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="saved UART log file")
    parser.add_argument("--max-startup-ms", type=int, default=5000)
    parser.add_argument("--max-recovery-window-ms", type=int, default=250)
    parser.add_argument("--expected-web-cache-base-priority", type=int, default=1)
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        content = args.log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"BOOT_RESPONSIVENESS_ANALYSIS: FAIL: cannot read {args.log}: {exc}", file=sys.stderr)
        return 1

    result = analyze(
        content,
        max_startup_ms=args.max_startup_ms,
        max_recovery_window_ms=args.max_recovery_window_ms,
        expected_web_cache_base_priority=args.expected_web_cache_base_priority,
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
