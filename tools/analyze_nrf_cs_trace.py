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

Cable bound (--cable-mm): the sensors hang on cables of a known length from MAIN (node 0), so a pair with MAIN cannot be farther
apart than one cable and two sensors cannot be farther than two cables. That is a physical truth that needs no tape measure:
for every pair and every method the report counts the estimates inside the bound. An estimate outside it is wrong by at least the
excess, whatever its spread.

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
TONES = re.compile(r" tones=(\d+)/(\d+) gap=(\d+)(?: lead=(\d+) tail=(\d+))?")
PEAK_FLOOR = re.compile(r" pf=(\d+)\.(\d)")
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
    tones: tuple[int, int, int] | None = None                      # (usable, planned, longest hole) of the frequency set
    edges: tuple[int, int] | None = None                           # (hole at the start, hole at the end) of the 75 slots
    peak_floor: float | None = None                                # IFFT peak over the median bin (sharpness), None in old logs
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
            sharp = PEAK_FLOOR.search(rest)
            if sharp:
                attempt.peak_floor = int(sharp.group(1)) + int(sharp.group(2)) / 10.0
            tones = TONES.search(rest)
            if tones:
                attempt.tones = (int(tones.group(1)), int(tones.group(2)), int(tones.group(3)))
                if tones.group(4) is not None:
                    attempt.edges = (int(tones.group(4)), int(tones.group(5)))
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


def pair_bound_mm(pair: tuple[int, int], cable_mm: int) -> int:
    """Largest possible distance of a pair when every sensor is on a cable of cable_mm from MAIN (node 0)."""
    return cable_mm if 0 in pair else 2 * cable_mm


def bound_check(attempts: dict[int, Attempt], cable_mm: int) -> list[str]:
    """Per pair and method: how many of the estimates lie inside the cable bound, and the median estimate."""
    by_pair: dict[tuple[int, int], dict[str, list[int]]] = defaultdict(lambda: defaultdict(list))
    for a in attempts.values():
        if a.pair is None or a.result != "SUCCESS":
            continue
        for method in ("ifft", "phase", "rtt"):
            if method in a.estimates:
                by_pair[a.pair][method].append(a.estimates[method])
    lines = []
    for pair, methods in sorted(by_pair.items()):
        bound = pair_bound_mm(pair, cable_mm)
        parts = []
        for method in ("ifft", "phase", "rtt"):
            values = sorted(methods.get(method, []))
            if values:
                inside = sum(1 for v in values if v <= bound)
                parts.append(f"{method} in={inside}/{len(values)} median={values[len(values) // 2]}")
        lines.append(f"   pair {pair[0]}-{pair[1]} bound {bound} mm: " + "; ".join(parts))
    return lines


def gap_table(attempts: dict[int, Attempt], cable_mm: int) -> list[str]:
    """CS-IFFT-GAP-01 from an ordinary log: does the IFFT go wrong when the frequency set has holes?

    Attempts are grouped by the longest hole; for each group the table gives how many there are, the median number of
    usable tones and, with the cable bound, how many IFFT estimates are inside it. If the false peaks come from the holes,
    the inside share falls as the hole grows."""
    groups: dict[str, list[Attempt]] = defaultdict(list)
    for a in attempts.values():
        if a.tones is None or a.result != "SUCCESS" or "ifft" not in a.estimates:
            continue
        gap = a.tones[2]
        groups["0" if gap == 0 else "1-2" if gap <= 2 else "3-5" if gap <= 5 else "6+"].append(a)
    lines = []
    for name in ("0", "1-2", "3-5", "6+"):
        members = groups.get(name, [])
        if not members:
            continue
        usable = sorted(a.tones[0] for a in members if a.tones)
        text = f"   hole {name:<4} n={len(members):<3} usable tones median={usable[len(usable) // 2]}"
        if cable_mm > 0:
            inside = sum(1 for a in members if a.pair is not None and a.estimates["ifft"] <= pair_bound_mm(a.pair, cable_mm))
            text += f"  ifft inside the cable bound {inside}/{len(members)}"
        lines.append(text)
    # A Wi-Fi access point on channel 1 (2412 MHz +-11) takes the first ~20 slots of the 75: a hole at the START of the array.
    # With the radio off (APP_WIFI_OFF_FOR_CS_TEST) the share of such attempts should fall to the level of the other end.
    with_edges = [a for a in attempts.values() if a.edges is not None and a.result == "SUCCESS"]
    if with_edges:
        start = sum(1 for a in with_edges if a.edges[0] >= 10)
        end = sum(1 for a in with_edges if a.edges[1] >= 10)
        lines.append(f"   holes of 10+ slots at the start of the array: {start}/{len(with_edges)}, at the end: {end}/{len(with_edges)}")
    return lines


SHARP_PEAK_RATIO = 10.0          # nrf_cs_ifft_quality.h: the value the Wieme replay analysis used


def sharpness_table(attempts: dict[int, Attempt], cable_mm: int) -> list[str]:
    """Does a sharp IFFT peak (peak over median bin above 10) separate the good estimates from the false ones on this board?

    On the Wieme replay the sharp ones (44% of line-of-sight 1-5 m estimates) had a 95th percentile error of 0.58 m against 2.35 m
    for all of them. Here the truth is unknown, so with --cable-mm the table gives how many IFFT estimates are inside the cable bound
    for the sharp and for the other attempts: if the sharp ones are inside far more often, the gate carries over to this board."""
    sharp = [a for a in attempts.values() if a.peak_floor is not None and a.result == "SUCCESS" and "ifft" in a.estimates]
    if not sharp:
        return []
    lines = []
    for name, members in (("sharp (>10)", [a for a in sharp if a.peak_floor > SHARP_PEAK_RATIO]),
                          ("not sharp ", [a for a in sharp if a.peak_floor <= SHARP_PEAK_RATIO])):
        if not members:
            continue
        values = sorted(a.peak_floor for a in members)
        text = f"   peak/floor {name} n={len(members):<4} median ratio={values[len(values) // 2]:.1f}"
        if cable_mm > 0:
            inside = sum(1 for a in members if a.pair is not None and a.estimates["ifft"] <= pair_bound_mm(a.pair, cable_mm))
            text += f"  ifft inside the cable bound {inside}/{len(members)}"
        lines.append(text)
    lines.append(f"   attempts with a sharp peak: {sum(1 for a in sharp if a.peak_floor > SHARP_PEAK_RATIO)}/{len(sharp)}")
    return lines


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
           truth: dict[tuple[int, int], int] | None = None, cable_mm: int = 0) -> list[str]:
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
    sharpness = sharpness_table(attempts, cable_mm)
    if sharpness:
        lines.append("   IFFT peak sharpness (peak over the median bin of the IFFT profile):")
        lines.extend(sharpness)
    holes = gap_table(attempts, cable_mm)
    if holes:
        lines.append("   frequency set holes (longest run of slots without a usable tone):")
        lines.extend(holes)
    if cable_mm > 0:
        lines.append(f"   estimates inside the cable bound (cable {cable_mm} mm from MAIN):")
        lines.extend(bound_check(attempts, cable_mm))
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
    parser.add_argument("--cable-mm", type=int, default=0, help="cable length of every sensor from MAIN, mm (physical upper bound)")
    args = parser.parse_args(argv)
    truth = parse_truth(args.truth)
    for path in args.logs:
        attempts, links = parse(path.read_bytes().decode("utf-8", "replace"))
        print("\n".join(report(str(path), attempts, links, truth, args.cable_mm)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
