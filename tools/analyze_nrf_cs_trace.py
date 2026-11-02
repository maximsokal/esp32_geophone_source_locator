#!/usr/bin/env python3
"""Stage times and distance accuracy of Channel Sounding attempts from the "cs:" lines of nRF UART logs (nrf_cs_trace.h).

Usage: analyze_nrf_cs_trace.py [--truth 0-1=1000,0-4=2500] LOG [LOG ...]   (MAIN and REMOTE logs may be given together)

Stage times: for every attempt the time of each stage is the time between two consecutive events of the attempt; the
budget of one pair has to cover the longest time of every stage that was measured. Nothing is suggested when no attempt
succeeded.

Accuracy: with the true distances of the pairs in mm (--truth, a pair written with any node order) it prints, for each
method of the distance library (ifft, phase slope, rtt and the one it picks as best), how far the measurements are from
the truth: the offset (mean error), the scatter (standard deviation) and the worst error left after the offset is removed.
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
WHO = re.compile(r"init=(\d+) refl=(\d+)")
ESTIMATE = re.compile(r"(ifft|phase|rtt|best)_mm=(-?\d+)")
MARGIN = 1.5
METHODS = ("ifft", "phase", "rtt", "best")
NOT_A_STAGE = ("EXECUTE_WORK_STARTED", "TRANSPORT_ERROR", "DISCONNECTED")


@dataclass
class Attempt:
    budget_ms: int = 0
    pair: tuple[int, int] | None = None                            # (smaller node id, larger node id)
    estimates: dict[str, int] = field(default_factory=dict)        # method -> mm; only the ones the library made
    events: list[tuple[str, int]] = field(default_factory=list)    # (event name, ms since the attempt was accepted)
    timeout_phase: str | None = None
    result: str | None = None
    total_ms: int | None = None


def _pair(first: int, second: int) -> tuple[int, int]:
    return (first, second) if first <= second else (second, first)


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
            who = WHO.search(rest)
            if who:
                attempt.pair = _pair(int(who.group(1)), int(who.group(2)))
        elif rest.startswith("est "):
            for name, value in ESTIMATE.findall(rest):
                attempt.estimates[name] = int(value)
        elif rest.startswith("ev=") and at is not None:
            attempt.events.append((rest.split()[0][3:], at))
        elif rest.startswith("TIMEOUT"):
            attempt.timeout_phase = re.search(r"phase=(\w+)", rest).group(1)
        elif rest.startswith("result="):
            attempt.result = rest.split()[0][7:]
            attempt.total_ms = at
    return attempts, links


def parse_truth(text: str) -> dict[tuple[int, int], int]:
    truth: dict[tuple[int, int], int] = {}
    for item in filter(None, text.split(",")):
        pair, _, mm = item.partition("=")
        low, _, high = pair.partition("-")
        truth[_pair(int(low), int(high))] = int(mm)
    return truth


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


def accuracy(attempts: dict[int, Attempt], truth: dict[tuple[int, int], int]) -> list[str]:
    """Per method: the offset, the scatter, and the worst error that stays after the offset is removed."""
    lines = []
    for method in METHODS:
        errors = [a.estimates[method] - truth[a.pair] for a in attempts.values()
                  if a.pair in truth and method in a.estimates]
        if not errors:
            lines.append(f"   {method:<6} no estimates at the given pairs")
            continue
        mean = sum(errors) / len(errors)
        stdev = (sum((e - mean) ** 2 for e in errors) / len(errors)) ** 0.5
        worst = max(abs(e - mean) for e in errors)
        lines.append(f"   {method:<6} n={len(errors):<3} offset={mean:>8.0f} mm  scatter={stdev:>6.0f} mm  "
                     f"worst after removing the offset +-{worst:.0f} mm")
    return lines


def report(path: str, attempts: dict[int, Attempt], links: list[int],
           truth: dict[tuple[int, int], int] | None = None) -> list[str]:
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
    if truth:
        lines.append("   accuracy against the given true distances:")
        lines.extend(accuracy(attempts, truth))
    done = [a.total_ms for a in attempts.values() if a.result == "SUCCESS" and a.total_ms is not None]
    if done:
        lines.append(f"   successful attempts took max {max(done)} ms: budget with margin {int(max(done) * MARGIN)} ms")
    else:
        lines.append("   no attempt succeeded: no budget can be derived from this log")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--truth", default="", help="true distances in mm, for example 0-1=1000,0-4=2500")
    args = parser.parse_args(argv)
    truth = parse_truth(args.truth)
    for path in args.logs:
        attempts, links = parse(path.read_bytes().decode("utf-8", "replace"))
        print("\n".join(report(str(path), attempts, links, truth)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
