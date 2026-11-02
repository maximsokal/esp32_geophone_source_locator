#!/usr/bin/env python3
"""Apply measurable load gates to an already-captured ESP32 UART log.

The analyzer never opens serial ports or touches hardware. It looks only at the
windows in which the AD7606 acquisition was running (``AD7606_ACQ: Started`` ..
``Stopped``) and evaluates gates G1-G7 from docs/acquisition-ui-load-plan-2026-11-01.md. Gate G0 looks at the
whole log: a boot that never reaches the application (incomplete flash, no bootable partition) or a
panic must FAIL even when no acquisition ran, instead of passing as "not applicable".
A log without an acquisition window reports every gate as NOT_APPLICABLE; that
is never a PASS, because a system that never ran the acquisition proves nothing
about it.

Exit code: 1 if any gate FAILs, 0 if nothing failed and at least one acquisition gate
(G1-G7) passed, 2 if nothing could be evaluated.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import re
import sys

PASS = "PASS"
FAIL = "FAIL"
NOT_APPLICABLE = "NOT_APPLICABLE"

# Thresholds come from the measured baselines in the plan (section 4).
DEFAULT_MIN_FPS_RATIO = 0.85
DEFAULT_MAX_RENDER_US = 100_000
DEFAULT_MAX_NODE_GAP_MS = 5_000
DEFAULT_MIN_IDLE1_SHARE = 0.15
DEFAULT_MIN_DELIVERY_RATIO = 0.99
DEFAULT_MAX_CORE1_LOAD = 0.80
DIAG_MIN_CONVERSIONS = 2_000
DIAG_SHORTFALL_RATIO = 0.5
UI_PERF_WARMUP_MS = 10_000
UI_PERF_TRAILING_MS = 500
IDLE1_MIN_OVERLAP_MS = 5_000

_ANSI = re.compile(rb"\x1b\[[0-9;]*m")
# Boot failures and panics; the plain text is searched because the ROM prints them without a timestamp.
_BOOT_FAILURES = (
    "No bootable app partitions",
    "Factory app partition is not bootable",
    "invalid segment length",
    "image hash",
    "Guru Meditation Error",
    "abort() was called",
)
_BOOT_OK = re.compile(r"I \(\d+\) (?:app_init|cpu_start): ")
# Lines can be glued together by the serial monitor, so tokenise the whole text.
_MARKER = re.compile(r"([EWI]) \((\d+)\) ([A-Za-z0-9_.-]+): ")


@dataclass(frozen=True)
class Entry:
    level: str
    t_ms: int
    tag: str
    message: str


@dataclass(frozen=True)
class Window:
    start_ms: int
    end_ms: int
    rate_hz: int
    channels: int


@dataclass
class Finding:
    gate: str
    scope: str
    status: str
    measured: str
    threshold: str


@dataclass
class Report:
    path: str
    windows: list[Window] = field(default_factory=list)
    findings: list[Finding] = field(default_factory=list)
    adc_words: dict[str, dict[str, int]] = field(default_factory=dict)
    adc_rate_channels: dict[str, dict[str, int]] = field(default_factory=dict)
    # Facts for the render and memory questions; they decide no gate.
    render_profile: dict[str, int] = field(default_factory=dict)
    mem_probe: dict[str, str] = field(default_factory=dict)
    mem_watch: list[dict[str, int]] = field(default_factory=list)

    def gate_status(self, gate: str) -> str:
        statuses = [f.status for f in self.findings if f.gate == gate]
        if FAIL in statuses:
            return FAIL
        if PASS in statuses:
            return PASS
        return NOT_APPLICABLE

    def gates(self) -> dict[str, str]:
        return {gate: self.gate_status(gate) for gate in ("G0", "G1", "G2", "G3", "G4", "G5", "G6", "G7", "G8", "G9")}

    def exit_code(self) -> int:
        gates = self.gates()
        if FAIL in gates.values():
            return 1
        # A clean boot alone proves nothing about the acquisition.
        return 0 if any(status == PASS for gate, status in gates.items() if gate != "G0") else 2


def clean_text(raw: bytes) -> str:
    return _ANSI.sub(b"", raw.replace(b"\x00", b"")).decode("utf-8", "replace")


def tokenize(text: str) -> list[Entry]:
    markers = list(_MARKER.finditer(text))
    entries: list[Entry] = []
    for index, marker in enumerate(markers):
        end = markers[index + 1].start() if index + 1 < len(markers) else len(text)
        message = text[marker.end():end].split("\n", 1)[0].rstrip("\r ")
        entries.append(Entry(marker.group(1), int(marker.group(2)), marker.group(3), message))
    return entries


def find_windows(entries: list[Entry]) -> list[Window]:
    windows: list[Window] = []
    current: tuple[int, int, int] | None = None
    last_t = entries[-1].t_ms if entries else 0
    for entry in entries:
        if entry.tag != "AD7606_ACQ":
            continue
        started = re.match(r"Started: (\d+) Hz, (\d+) channels", entry.message)
        if started:
            current = (entry.t_ms, int(started.group(1)), int(started.group(2)))
        elif entry.message.startswith("Stopped") and current is not None:
            windows.append(Window(current[0], entry.t_ms, current[1], current[2]))
            current = None
    if current is not None:
        windows.append(Window(current[0], max(last_t, current[0]), current[1], current[2]))
    return windows


def _inside(t_ms: int, window: Window, lo: int = 0, hi: int = 0) -> bool:
    return window.start_ms + lo <= t_ms <= window.end_ms + hi


def _scope(window: Window) -> str:
    return f"{window.start_ms / 1000:.1f}s-{window.end_ms / 1000:.1f}s"


def _gate_g1(entries: list[Entry], window: Window) -> Finding:
    count = sum(
        1 for e in entries
        if e.tag == "task_wdt" and e.message.startswith("Task watchdog got triggered")
        and _inside(e.t_ms, window)
    )
    return Finding("G1", _scope(window), PASS if count == 0 else FAIL, f"{count} task_wdt events", "0")


def _gate_g2(entries: list[Entry], window: Window, min_ratio: float, max_render_us: int) -> Finding:
    threshold = f"fps>={min_ratio:.2f}*target, avg_render<={max_render_us}us"
    perf = []
    for e in entries:
        if e.tag != "UI" or not e.message.startswith("UI_PERF"):
            continue
        if not _inside(e.t_ms, window, UI_PERF_WARMUP_MS, UI_PERF_TRAILING_MS):
            continue
        fields = dict(re.findall(r"(\w+)=([0-9.]+)", e.message))
        if {"target_fps", "actual_fps", "avg_render_us"} <= fields.keys():
            perf.append((float(fields["target_fps"]), float(fields["actual_fps"]), int(fields["avg_render_us"])))
    if not perf:
        return Finding("G2", _scope(window), NOT_APPLICABLE, "no UI_PERF after warm-up", threshold)
    worst_ratio = min(actual / target for target, actual, _ in perf if target > 0)
    worst_render = max(render for _, _, render in perf)
    ok = worst_ratio >= min_ratio and worst_render <= max_render_us
    return Finding(
        "G2", _scope(window), PASS if ok else FAIL,
        f"min fps ratio {worst_ratio:.2f}, max avg_render {worst_render}us over {len(perf)} reports", threshold)


def _gate_g3(entries: list[Entry], window: Window, max_gap_ms: int) -> list[Finding]:
    by_node: dict[int, list[int]] = {}
    for e in entries:
        match = re.match(r"NRF_LOG_RX node=(\d+) ", e.message) if e.tag == "NRF_TOPOLOGY" else None
        if match:
            by_node.setdefault(int(match.group(1)), []).append(e.t_ms)
    findings = []
    for node in sorted(by_node):
        stamps = sorted(by_node[node])
        if stamps[0] > window.end_ms:
            continue
        points = [window.start_ms] + [t for t in stamps if window.start_ms < t <= window.end_ms] + [window.end_ms]
        gap = max(b - a for a, b in zip(points, points[1:]))
        findings.append(Finding(
            "G3", f"{_scope(window)} node{node}", PASS if gap <= max_gap_ms else FAIL,
            f"max gap {gap}ms", f"<={max_gap_ms}ms"))
    return findings


def _gate_g4(entries: list[Entry], window: Window) -> Finding:
    stale = 0
    reports = 0
    for e in entries:
        if e.tag != "NRF_TOPOLOGY" or "generation=" not in e.message or not _inside(e.t_ms, window):
            continue
        match = re.search(r"stale=(\d+)", e.message)
        if match:
            reports += 1
            stale += match.group(1) == "1"
    if reports == 0:
        return Finding("G4", _scope(window), NOT_APPLICABLE, "no topology report", "stale=0")
    return Finding("G4", _scope(window), PASS if stale == 0 else FAIL,
                   f"{stale} of {reports} topology reports stale", "0 stale")


def _gate_g5(entries: list[Entry], windows: list[Window], min_share: float) -> list[Finding]:
    findings = []
    for index, entry in enumerate(entries):
        if entry.tag != "RUNTIME_OBSERVABILITY" or not entry.message.startswith("RUNTIME_CPU"):
            continue
        fields = dict(re.findall(r"(\w+)=([0-9.]+)", entry.message))
        if fields.get("valid") != "1" or "window_ms" not in fields:
            continue
        span_ms = int(fields["window_ms"])
        begin = entry.t_ms - span_ms
        overlap = sum(max(0, min(entry.t_ms, w.end_ms) - max(begin, w.start_ms)) for w in windows)
        if overlap < IDLE1_MIN_OVERLAP_MS:
            continue
        idle_us = None
        for follower in entries[index + 1:]:
            if follower.tag == "RUNTIME_OBSERVABILITY" and follower.message.startswith("RUNTIME_CPU"):
                break
            task = re.match(r"RUNTIME_TASK .*?name=IDLE1 core=1 .*?runtime_delta_us=(\d+)", follower.message)
            if follower.tag == "RUNTIME_OBSERVABILITY" and task:
                idle_us = int(task.group(1))
                break
        scope = f"report@{entry.t_ms / 1000:.1f}s overlap {overlap / 1000:.1f}s"
        threshold = f">={min_share * 100:.0f}% (estimate)"
        if idle_us is None:
            findings.append(Finding("G5", scope, NOT_APPLICABLE, "no IDLE1 line", threshold))
            continue
        share = max(0, idle_us - (span_ms - overlap) * 1000) / (overlap * 1000)
        findings.append(Finding("G5", scope, PASS if share >= min_share else FAIL,
                                f"IDLE1 estimate {share * 100:.1f}% during ACQ", threshold))
    return findings


def _fields(message: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=([0-9.]+)", message))


def _rate_lines(entries: list[Entry], window: Window) -> list[dict[str, int]]:
    """AD7606_RATE reports (one per second, printed by the service core) inside a window."""
    lines = []
    for e in entries:
        if e.tag != "AD7606_RATE" or not e.message.startswith("AD7606_RATE:") or not _inside(e.t_ms, window):
            continue
        fields = {k: int(v) for k, v in _fields(e.message).items() if v.isdigit()}
        match = re.search(r"\bisr=(\d+)/(\d+)/(\d+)", e.message)
        if match:
            fields["isr_n"], fields["isr_cyc_avg"], fields["isr_cyc_max"] = (int(g) for g in match.groups())
        for name, key in (("loop", "loop_cyc"), ("spi", "spi_cyc"), ("pub", "pub_cyc")):
            match = re.search(r"\b" + name + r"=(\d+)/(\d+)", e.message)
            if match:
                fields[key + "_avg"], fields[key + "_max"] = int(match.group(1)), int(match.group(2))
        for name in ("p2p", "rms10", "mean", "sat"):
            match = re.search(r"\b" + name + r"=(-?\d+)/(-?\d+)/(-?\d+)/(-?\d+)", e.message)
            if match:
                fields[name] = [int(g) for g in match.groups()]
        lines.append(fields)
    return lines


def _gate_g6(entries: list[Entry], window: Window, min_ratio: float) -> Finding:
    """Frames that reach the ring buffer against the conversions the ADC completed."""
    threshold = f"published>={min_ratio * 100:.0f}% of conversions"
    rates = [r for r in _rate_lines(entries, window) if r.get("conv", 0) > 0]
    if rates:
        worst = min(r["published"] / r["conv"] for r in rates)
        return Finding("G6", _scope(window), PASS if worst >= min_ratio else FAIL,
                       f"worst second {worst * 100:.1f}% over {len(rates)} AD7606_RATE reports", threshold)
    # Logs from before AD7606_RATE: SERIAL_DIAG prints the conversion count (samples) next to the
    # number of frames the worker actually observed (window). Only a clear shortfall is evidence.
    shortfall = None
    for e in entries:
        if e.tag != "AD7606_DIAG" or not e.message.startswith("AD7606_SERIAL_DIAG") or not _inside(e.t_ms, window):
            continue
        samples = re.search(r"\bsamples=(\d+)", e.message)
        observed = re.search(r"\bwindow=(\d+)", e.message)
        if samples and observed and int(samples.group(1)) >= DIAG_MIN_CONVERSIONS:
            ratio = int(observed.group(1)) / int(samples.group(1))
            if ratio < DIAG_SHORTFALL_RATIO and (shortfall is None or ratio < shortfall[0]):
                shortfall = (ratio, int(observed.group(1)), int(samples.group(1)))
    if shortfall:
        return Finding("G6", _scope(window), FAIL,
                       f"estimate: {shortfall[1]} frames observed of {shortfall[2]} conversions "
                       f"({shortfall[0] * 100:.1f}%, from AD7606_SERIAL_DIAG)", threshold)
    return Finding("G6", _scope(window), NOT_APPLICABLE, "no AD7606_RATE report", threshold)


def _gate_g7(entries: list[Entry], window: Window, max_load: float) -> Finding:
    """Core 1 load estimated by the firmware's own cycle counters (ISR + worker wake-ups)."""
    threshold = f"<={max_load * 100:.0f}% of the CPU"
    loads = []
    for r in _rate_lines(entries, window):
        if r.get("window_ms", 0) > 0 and r.get("cpu_mhz", 0) > 0 and "loop_cyc_avg" in r:
            cycles = r.get("isr_n", 0) * r.get("isr_cyc_avg", 0) + r.get("loops", 0) * r["loop_cyc_avg"]
            loads.append(cycles / (r["cpu_mhz"] * 1000 * r["window_ms"]))
    if not loads:
        return Finding("G7", _scope(window), NOT_APPLICABLE, "no AD7606_RATE report", threshold)
    worst = max(loads)
    return Finding("G7", _scope(window), PASS if worst <= max_load else FAIL,
                   f"worst second {worst * 100:.1f}% over {len(loads)} reports", threshold)


def _adc_rate_channels(entries: list[Entry], windows: list[Window]) -> dict[str, dict[str, int]]:
    """Per-channel peak-to-peak and RMS (tenths of a code) from AD7606_RATE lines, if the firmware prints them."""
    result: dict[str, dict[str, int]] = {}
    for window in windows:
        for line in _rate_lines(entries, window):
            rms = line.get("rms10", [])
            # A line cut short by the serial link still counts for the gates; its channel fields may be missing.
            for index, value in enumerate(line.get("p2p", [])):
                stats = result.setdefault(f"ch{index + 1}", {"n": 0, "p2p_min": value, "p2p_max": value, "rms10_max": 0})
                stats["n"] += 1
                stats["p2p_min"] = min(stats["p2p_min"], value)
                stats["p2p_max"] = max(stats["p2p_max"], value)
                if index < len(rms):
                    stats["rms10_max"] = max(stats["rms10_max"], rms[index])
    return result


def _adc_words(entries: list[Entry], windows: list[Window]) -> dict[str, dict[str, int]]:
    samples: list[list[int]] = []
    for e in entries:
        match = re.search(r"words=\[([-0-9,]+)\]", e.message) if e.tag == "AD7606_DIAG" else None
        if match and any(_inside(e.t_ms, w) for w in windows):
            samples.append([int(v) for v in match.group(1).split(",")])
    result: dict[str, dict[str, int]] = {}
    for channel in range(max((len(s) for s in samples), default=0)):
        values = [s[channel] for s in samples if len(s) > channel]
        result[f"ch{channel + 1}"] = {"n": len(values), "min": min(values), "max": max(values),
                                      "range": max(values) - min(values)}
    return result


def _gate_g0(text: str) -> Finding:
    """The firmware must boot into the application and must not panic."""
    found = sorted({marker for marker in _BOOT_FAILURES if marker in text})
    if found:
        count = sum(text.count(marker) for marker in found)
        return Finding("G0", "whole log", FAIL, f"{count} boot/panic lines: " + "; ".join(found), "none")
    if _BOOT_OK.search(text):
        return Finding("G0", "whole log", PASS, "application started, no boot failure or panic", "none")
    return Finding("G0", "whole log", NOT_APPLICABLE, "no boot lines in this log", "none")


_PLACEMENT_LINE = re.compile(r"PLACEMENT_STABILITY node=(\S+) stable=(\d) reason=(\S+) trigger=(\S+)")
_RANGING_RESULT = re.compile(r"RANGING_RESULT success=(\d) partial=(\d) reason=(\S+) executions=(\d+)/(\d+) pairs=(\d+)")
DEFAULT_MAX_PLACEMENT_MS = 40000


def _gate_g8(entries: list[Entry], max_ms: int) -> Finding:
    """The placement step must settle in time: every node stable at once, not wiped by a blind MAIN barometer."""
    first_ms = None
    latest: dict[str, bool] = {}
    settled_ms = None
    barometer_resets = 0
    for entry in entries:
        if entry.tag != "WORKFLOW":
            continue
        match = _PLACEMENT_LINE.match(entry.message)
        if match is None:
            continue
        node, stable, reason, trigger = match.group(1), match.group(2) == "1", match.group(3), match.group(4)
        if first_ms is None:
            first_ms = entry.t_ms
        if reason == "main_barometer" and trigger == "change" and node == "MAIN":
            barometer_resets += 1
        latest[node] = stable
        if settled_ms is None and len(latest) >= 2 and all(latest.values()):
            settled_ms = entry.t_ms
    if first_ms is None:
        return Finding("G8", "placement", NOT_APPLICABLE, "no PLACEMENT_STABILITY lines", f"<={max_ms // 1000}s")
    resets = f", {barometer_resets} MAIN barometer interruptions" if barometer_resets else ""
    if settled_ms is None:
        return Finding("G8", "placement", FAIL, f"never settled in {(entry_end(entries) - first_ms) / 1000:.1f}s{resets}",
                       f"<={max_ms // 1000}s")
    took = settled_ms - first_ms
    return Finding("G8", f"placement {first_ms / 1000:.1f}s", PASS if took <= max_ms else FAIL,
                   f"all nodes stable after {took / 1000:.1f}s{resets}", f"<={max_ms // 1000}s")


def entry_end(entries: list[Entry]) -> int:
    return entries[-1].t_ms if entries else 0


def _gate_g9(entries: list[Entry]) -> Finding:
    """The BLE Channel Sounding positions must be solved: the last RANGING_RESULT of the log decides."""
    last = None
    for entry in entries:
        if entry.tag != "WORKFLOW":
            continue
        match = _RANGING_RESULT.match(entry.message)
        if match is not None:
            last = (entry.t_ms, match)
    if last is None:
        return Finding("G9", "ranging", NOT_APPLICABLE, "no RANGING_RESULT line (older firmware?)", "success")
    t_ms, match = last
    success, partial, reason = match.group(1) == "1", match.group(2) == "1", match.group(3)
    measured = f"success, {match.group(6)} pairs" if success else (
        f"failed: {reason}{' (partial pairs saved)' if partial else ''}, executions {match.group(4)}/{match.group(5)}")
    return Finding("G9", f"ranging {t_ms / 1000:.1f}s", PASS if success else FAIL, measured, "success")


def _render_profile(entries: list[Entry]) -> dict[str, int]:
    """UI_PROFILE windows combined: what a frame is made of (chunks) and what a chunk costs (time, pixels)."""
    frames = chunks_x10 = chunk_us = pixels = 0
    reports = 0
    out = {"chunks_per_frame_max": 0, "chunk_draw_max_us": 0, "scene_objects_max": 0}
    for e in entries:
        if e.tag != "UI" or not e.message.startswith("UI_PROFILE:"):
            continue
        f = {k: int(float(v)) for k, v in _fields(e.message).items()}
        if not {"frames", "chunks_per_frame_x10", "chunk_draw_avg_us", "pixels_per_chunk"} <= f.keys() or f["frames"] <= 0:
            continue
        reports += 1
        window_chunks_x10 = f["frames"] * f["chunks_per_frame_x10"]        # chunks * 10
        frames += f["frames"]
        chunks_x10 += window_chunks_x10
        chunk_us += f["chunk_draw_avg_us"] * window_chunks_x10             # (us * chunks) * 10
        pixels += f["pixels_per_chunk"] * window_chunks_x10
        out["chunks_per_frame_max"] = max(out["chunks_per_frame_max"], f.get("chunks_per_frame_max", 0))
        out["chunk_draw_max_us"] = max(out["chunk_draw_max_us"], f.get("chunk_draw_max_us", 0))
        out["scene_objects_max"] = max(out["scene_objects_max"], f.get("scene_objects", 0))
    if reports == 0 or chunks_x10 == 0:
        return {}
    out.update(reports=reports, frames=frames,
               chunks_per_frame_x10=chunks_x10 // frames,
               chunk_draw_avg_us=chunk_us // chunks_x10,
               pixels_per_chunk=pixels // chunks_x10,
               draw_ns_per_pixel=chunk_us * 1000 // pixels if pixels else 0)
    return out


def _mem_probe(entries: list[Entry]) -> dict[str, str]:
    for e in entries:
        if e.tag == "UI_LVGL" and e.message.startswith("UI_MEM_PROBE"):
            return dict(re.findall(r"(\w+)=(\S+)", e.message))
    return {}


def _mem_watch(entries: list[Entry]) -> list[dict[str, int]]:
    events = []
    for e in entries:
        match = re.match(r"MEM_WATCH dma_free=(\d+)->(\d+) .*?tasks=(\d+)", e.message) if e.tag == "RUNTIME_OBSERVABILITY" else None
        if match:
            events.append({"t_ms": e.t_ms, "from": int(match.group(1)), "to": int(match.group(2)),
                           "tasks": int(match.group(3))})
    return events


def analyze_text(text: str, path: str = "<memory>", *, min_fps_ratio: float = DEFAULT_MIN_FPS_RATIO,
                 max_render_us: int = DEFAULT_MAX_RENDER_US, max_node_gap_ms: int = DEFAULT_MAX_NODE_GAP_MS,
                 min_idle1_share: float = DEFAULT_MIN_IDLE1_SHARE,
                 min_delivery_ratio: float = DEFAULT_MIN_DELIVERY_RATIO,
                 max_core1_load: float = DEFAULT_MAX_CORE1_LOAD,
                 max_placement_ms: int = DEFAULT_MAX_PLACEMENT_MS) -> Report:
    entries = tokenize(text)
    report = Report(path=path, windows=find_windows(entries))
    report.findings.append(_gate_g0(text))
    for window in report.windows:
        report.findings.append(_gate_g1(entries, window))
        report.findings.append(_gate_g2(entries, window, min_fps_ratio, max_render_us))
        report.findings.extend(_gate_g3(entries, window, max_node_gap_ms))
        report.findings.append(_gate_g4(entries, window))
        report.findings.append(_gate_g6(entries, window, min_delivery_ratio))
        report.findings.append(_gate_g7(entries, window, max_core1_load))
    report.findings.extend(_gate_g5(entries, report.windows, min_idle1_share))
    report.findings.append(_gate_g8(entries, max_placement_ms))
    report.findings.append(_gate_g9(entries))
    report.render_profile = _render_profile(entries)
    report.mem_probe = _mem_probe(entries)
    report.mem_watch = _mem_watch(entries)
    report.adc_words = _adc_words(entries, report.windows)
    report.adc_rate_channels = _adc_rate_channels(entries, report.windows)
    return report


def analyze_file(path: Path, **options) -> Report:
    return analyze_text(clean_text(path.read_bytes()), str(path), **options)


def format_report(report: Report) -> str:
    lines = [f"== {report.path}"]
    if not report.windows:
        lines.append("   no AD7606 acquisition window: the acquisition gates are NOT_APPLICABLE")
    for window in report.windows:
        lines.append(f"   ACQ {_scope(window)} {window.rate_hz} Hz, {window.channels} channels")
    for finding in report.findings:
        lines.append(f"   {finding.gate} {finding.status:<14} [{finding.scope}] {finding.measured} (limit {finding.threshold})")
    for name, stats in report.adc_words.items():
        lines.append(f"   info {name}: n={stats['n']} range={stats['range']} min={stats['min']} max={stats['max']}")
    for name, stats in report.adc_rate_channels.items():
        lines.append(f"   info {name} (AD7606_RATE): n={stats['n']} p2p={stats['p2p_min']}..{stats['p2p_max']} "
                     f"rms10<={stats['rms10_max']}")
    p = report.render_profile
    if p:
        lines.append(
            f"   info render (UI_PROFILE, {p['reports']} windows, {p['frames']} frames): "
            f"{p['chunks_per_frame_x10'] / 10:.1f} chunks/frame (max {p['chunks_per_frame_max']}), "
            f"{p['chunk_draw_avg_us'] / 1000:.1f} ms/chunk (max {p['chunk_draw_max_us'] / 1000:.1f}), "
            f"{p['pixels_per_chunk']} px/chunk, {p['draw_ns_per_pixel']} ns/px, scene objects {p['scene_objects_max']}")
    if report.mem_probe:
        lines.append("   info memory probe (UI_MEM_PROBE): " + " ".join(f"{k}={v}" for k, v in report.mem_probe.items()))
    for event in report.mem_watch:
        lines.append(f"   info MEM_WATCH {event['t_ms'] / 1000:.1f}s: dma_free {event['from']} -> {event['to']} "
                     f"({event['to'] - event['from']:+d}), {event['tasks']} tasks")
    lines.append("   gates: " + ", ".join(f"{g}={s}" for g, s in report.gates().items()))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="write machine-readable results here")
    parser.add_argument("--min-fps-ratio", type=float, default=DEFAULT_MIN_FPS_RATIO)
    parser.add_argument("--max-render-us", type=int, default=DEFAULT_MAX_RENDER_US)
    parser.add_argument("--max-node-gap-ms", type=int, default=DEFAULT_MAX_NODE_GAP_MS)
    parser.add_argument("--min-idle1-share", type=float, default=DEFAULT_MIN_IDLE1_SHARE)
    parser.add_argument("--min-delivery-ratio", type=float, default=DEFAULT_MIN_DELIVERY_RATIO)
    parser.add_argument("--max-core1-load", type=float, default=DEFAULT_MAX_CORE1_LOAD)
    parser.add_argument("--max-placement-ms", type=int, default=DEFAULT_MAX_PLACEMENT_MS)
    args = parser.parse_args(argv)
    options = dict(min_fps_ratio=args.min_fps_ratio, max_render_us=args.max_render_us,
                   max_node_gap_ms=args.max_node_gap_ms, min_idle1_share=args.min_idle1_share,
                   min_delivery_ratio=args.min_delivery_ratio, max_core1_load=args.max_core1_load,
                   max_placement_ms=args.max_placement_ms)
    reports = [analyze_file(path, **options) for path in args.logs]
    for report in reports:
        print(format_report(report))
    if args.json:
        args.json.write_text(json.dumps([
            {"path": r.path, "windows": [asdict(w) for w in r.windows], "gates": r.gates(),
             "findings": [asdict(f) for f in r.findings], "adc_words": r.adc_words,
             "adc_rate_channels": r.adc_rate_channels, "render_profile": r.render_profile,
             "mem_probe": r.mem_probe, "mem_watch": r.mem_watch}
            for r in reports], indent=2) + "\n", encoding="utf-8")
    codes = [r.exit_code() for r in reports]
    return 1 if 1 in codes else 0 if 0 in codes else 2


if __name__ == "__main__":
    sys.exit(main())
