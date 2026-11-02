#!/usr/bin/env python3
"""Stage times and distance accuracy of Channel Sounding attempts from the "cs:" lines of nRF UART logs (nrf_cs_trace.h).

Usage: analyze_nrf_cs_trace.py [--truth 0-1=1000,0-4=2500] LOG [LOG ...]   (MAIN and REMOTE logs may be given together)

Stage times: for every attempt the time of each stage is the time between two consecutive events of the attempt; the
budget of one pair has to cover the longest time of every stage that was measured. Nothing is suggested when no attempt
succeeded.

Methods (CS-ESTIMATOR-01): every attempt carries the three estimates of the distance library and the method "best" was
taken from. The report counts the methods used, and for each attempt the span between the estimates it compared: a
large span means the methods contradict each other on one raw procedure, so none of them can be trusted blindly.

Time (CS-TIME-01): a result line says when the distance was ready ("measured") and how long the disconnect took
("cleanup"); attempts that ran past their budget or whose budget timer fired during cleanup are listed apart from real
timeouts. TRANSPORT_ERROR events are counted by the reason the firmware names (CS-SECURITY-01).

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
METHOD = re.compile(r" method=(\w+)")
TIMING = re.compile(r" measured=(\d+)ms cleanup=(\d+)ms")
ERROR_REASON = re.compile(r" err=(\w+)")
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
    method: str | None = None                                      # the estimate "best" was taken from
    measured_ms: int | None = None                                 # until the outcome was known
    cleanup_ms: int | None = None                                  # the disconnect after it
    over_budget: bool = False
    timeout_in_cleanup: bool = False
    error_reasons: list[str] = field(default_factory=list)         # names of TRANSPORT_ERROR statuses


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
            method = METHOD.search(rest)
            if method:
                attempt.method = method.group(1)
        elif rest.startswith("ev=") and at is not None:
            attempt.events.append((rest.split()[0][3:], at))
            reason = ERROR_REASON.search(rest)
            if reason and rest.startswith("ev=TRANSPORT_ERROR"):
                attempt.error_reasons.append(reason.group(1))
        elif rest.startswith("TIMEOUT"):
            attempt.timeout_phase = re.search(r"phase=(\w+)", rest).group(1)
        elif rest.startswith("result="):
            attempt.result = rest.split()[0][7:]
            attempt.total_ms = at
            timing = TIMING.search(rest)
            if timing:
                attempt.measured_ms, attempt.cleanup_ms = int(timing.group(1)), int(timing.group(2))
            attempt.over_budget = " over_budget=1" in rest
            attempt.timeout_in_cleanup = " timeout_in_cleanup=1" in rest
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


def methods_used(attempts: dict[int, Attempt]) -> Counter:
    """How often each estimate was the one the distance came from (attempts without an est line are not counted)."""
    return Counter(a.method or "unknown" for a in attempts.values() if a.estimates)


def estimate_spans(attempts: dict[int, Attempt]) -> list[int]:
    """Per attempt: largest minus smallest of the ifft, phase and rtt estimates that exist (at least two are needed)."""
    spans = []
    for a in attempts.values():
        values = [a.estimates[m] for m in ("ifft", "phase", "rtt") if m in a.estimates]
        if len(values) >= 2:
            spans.append(max(values) - min(values))
    return spans


def per_pair_scatter(attempts: dict[int, Attempt]) -> dict[tuple[int, int], list[int]]:
    """Distances of the attempts that ended in SUCCESS, by pair (the direction does not matter)."""
    groups: dict[tuple[int, int], list[int]] = defaultdict(list)
    for a in attempts.values():
        if a.pair is not None and a.result == "SUCCESS" and "best" in a.estimates:
            groups[a.pair].append(a.estimates["best"])
    return groups


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
    used = methods_used(attempts)
    if used:
        lines.append("   method used for the distance: " + ", ".join(f"{k}={v}" for k, v in sorted(used.items())))
        spans = sorted(estimate_spans(attempts))
        if spans:
            lines.append(f"   span between the estimates of one procedure: min={spans[0]} median={spans[len(spans) // 2]} "
                         f"max={spans[-1]} mm; above 1000 mm in {sum(1 for s in spans if s > 1000)} of {len(spans)}")
    for pair, values in sorted(per_pair_scatter(attempts).items()):
        mean = sum(values) / len(values)
        stdev = (sum((v - mean) ** 2 for v in values) / len(values)) ** 0.5
        lines.append(f"   pair {pair[0]}-{pair[1]}: n={len(values)} min={min(values)} max={max(values)} "
                     f"mean={mean:.0f} sd={stdev:.0f} mm")
    late = [a for a in attempts.values() if a.over_budget or a.timeout_in_cleanup]
    if late:
        worst = max((a.cleanup_ms or 0) for a in late)
        lines.append(f"   attempts past their budget: {len(late)} (budget timer fired during cleanup in "
                     f"{sum(1 for a in late if a.timeout_in_cleanup)}), longest cleanup {worst} ms")
    reasons = Counter(r for a in attempts.values() for r in a.error_reasons)
    if reasons:
        lines.append("   transport errors by reason: " + ", ".join(f"{k}={v}" for k, v in sorted(reasons.items())))
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
