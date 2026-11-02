#!/usr/bin/env python3
"""Stage times of Channel Sounding attempts from the "cs:" lines of nRF UART logs (nrf_cs_trace.h).

Usage: analyze_nrf_cs_trace.py LOG [LOG ...]   (MAIN and REMOTE logs may be given together)

For every attempt the time of each stage is the time between two consecutive events of the attempt; the budget of one
pair has to cover the longest time of every stage that was measured. Nothing is suggested when no attempt succeeded.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys

LINE = re.compile(r"^cs: t=(\d+) a=(\d+) (.*)$")
ELAPSED = re.compile(r" \+(\d+)ms$")
MARGIN = 1.5
NOT_A_STAGE = ("EXECUTE_WORK_STARTED", "TRANSPORT_ERROR", "DISCONNECTED")


@dataclass
class Attempt:
    budget_ms: int = 0
    events: list[tuple[str, int]] = field(default_factory=list)   # (event name, ms since the attempt was accepted)
    timeout_phase: str | None = None
    result: str | None = None
    total_ms: int | None = None


def parse(text: str) -> tuple[dict[int, Attempt], list[int]]:
    """(attempts by id, durations of the links this node served as reflector)."""
    attempts: dict[int, Attempt] = {}
    links: list[int] = []
    link_start: int | None = None
    for raw in text.splitlines():
        match = LINE.match(raw.strip())
        if not match:
            continue
        t_ms, attempt_id, rest = int(match.group(1)), int(match.group(2)), match.group(3)
        elapsed = ELAPSED.search(rest)
        at = int(elapsed.group(1)) if elapsed else None
        if attempt_id == 0:
            if rest.startswith("ev=INCOMING_CONNECTED"):
                link_start = t_ms
            elif rest.startswith("ev=INCOMING_DISCONNECTED") and link_start is not None:
                links.append(t_ms - link_start)
                link_start = None
            continue
        attempt = attempts.setdefault(attempt_id, Attempt())
        if rest.startswith("execute"):
            budget = re.search(r"budget=(\d+)ms", rest)
            attempt.budget_ms = int(budget.group(1)) if budget else 0
        elif rest.startswith("ev=") and at is not None:
            attempt.events.append((rest.split()[0][3:], at))
        elif rest.startswith("TIMEOUT"):
            attempt.timeout_phase = re.search(r"phase=(\w+)", rest).group(1)
        elif rest.startswith("result="):
            attempt.result = rest.split()[0][7:]
            attempt.total_ms = at
    return attempts, links


def stage_times(attempts: dict[int, Attempt]) -> dict[str, list[int]]:
    """Milliseconds each stage took: the time from the previous event to the event that ended the stage."""
    stages: dict[str, list[int]] = defaultdict(list)
    for attempt in attempts.values():
        previous = 0
        for name, at in attempt.events:
            if name not in NOT_A_STAGE:
                stages[name].append(at - previous)
            previous = at
    return stages


def report(path: str, attempts: dict[int, Attempt], links: list[int]) -> list[str]:
    lines = [f"== {path}: {len(attempts)} attempts as initiator, {len(links)} links as reflector"]
    if not attempts:
        return lines + ['   no "cs:" lines (firmware without the console trace?)']
    lines.append("   results: " + ", ".join(f"{k}={v}" for k, v in Counter(a.result for a in attempts.values()).items()))
    cut = Counter(a.timeout_phase for a in attempts.values() if a.timeout_phase)
    lines.append("   cut in phase: " + ", ".join(f"{k}={v}" for k, v in cut.items()))
    for name, values in stage_times(attempts).items():
        values.sort()
        lines.append(f"   stage -> {name:<18} n={len(values):<3} min={values[0]:>5} median={values[len(values) // 2]:>5} "
                     f"max={values[-1]:>5} ms")
    if links:
        links.sort()
        lines.append(f"   reflector link lasted min={links[0]} median={links[len(links) // 2]} max={links[-1]} ms")
    done = [a.total_ms for a in attempts.values() if a.result == "SUCCESS" and a.total_ms is not None]
    if done:
        lines.append(f"   successful attempts took max {max(done)} ms: budget with margin {int(max(done) * MARGIN)} ms")
    else:
        lines.append("   no attempt succeeded: no budget can be derived from this log")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args(argv)
    for path in args.logs:
        attempts, links = parse(path.read_bytes().decode("utf-8", "replace"))
        print("\n".join(report(str(path), attempts, links)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
