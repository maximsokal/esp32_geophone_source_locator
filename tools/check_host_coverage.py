#!/usr/bin/env python3
"""Line-coverage gate for hardware-facing host-tested C files (MAX-247).

Run the host tests with ``--coverage`` first (one or more build dirs), then:

    python tools/check_host_coverage.py --build-root build/host-cov \
        --config tests/host/coverage-gate.json [--gcov "llvm-cov gcov"]

Every ``*.gcda`` under the build roots is summarised with ``gcov -n``. A
file compiled into several test binaries gets the BEST line coverage of
any one binary -- a conservative lower bound of the merged coverage, so
the gate never passes on coverage that does not exist. Each file listed in
the config must reach its minimum; the minimums are a ratchet set a little
below the measured values, raise them when coverage improves.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

SUMMARY = re.compile(r"File '([^']+)'\s*\nLines executed:\s*([\d.]+)% of (\d+)")


def parse_gcov_summary(text: str) -> dict[str, tuple[float, int]]:
    """Map every source in `gcov -n` output to (line percent, line count)."""
    result: dict[str, tuple[float, int]] = {}
    for match in SUMMARY.finditer(text):
        path = match.group(1).replace("\\", "/")
        result[path] = (float(match.group(2)), int(match.group(3)))
    return result


def best_coverage(summaries: list[dict[str, tuple[float, int]]], suffix: str) -> tuple[float, int] | None:
    """Best (percent, lines) of any summary whose path ends with `suffix`."""
    best: tuple[float, int] | None = None
    for summary in summaries:
        for path, value in summary.items():
            if path.endswith(suffix) and (best is None or value[0] > best[0]):
                best = value
    return best


def evaluate(config: dict[str, float], summaries: list[dict[str, tuple[float, int]]]) -> list[str]:
    """Return one failure line per file below its minimum or never covered."""
    failures = []
    for suffix, minimum in sorted(config.items()):
        best = best_coverage(summaries, suffix)
        if best is None:
            failures.append(f"{suffix}: no coverage data (not compiled into any host test?)")
        elif best[0] + 1e-9 < minimum:
            failures.append(f"{suffix}: {best[0]:.2f}% < required {minimum:.2f}%")
    return failures


def collect(build_roots: list[Path], gcov: list[str]) -> list[dict[str, tuple[float, int]]]:
    summaries = []
    for root in build_roots:
        for gcda in sorted(root.rglob("*.gcda")):
            completed = subprocess.run(
                [*gcov, "-n", str(gcda.resolve())],
                capture_output=True,
                text=True,
                cwd=gcda.parent,
                check=False,
            )
            summaries.append(parse_gcov_summary(completed.stdout))
    return summaries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-root", action="append", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--gcov", default="gcov", help='gcov command, e.g. "llvm-cov gcov"')
    args = parser.parse_args(argv)

    config = json.loads(args.config.read_text(encoding="utf-8"))["minimum_line_percent"]
    gcov = shlex.split(args.gcov, posix=True)
    if len(gcov) == 1 and Path(gcov[0]).name.startswith("llvm-cov"):
        gcov.append("gcov")
    summaries = collect(args.build_root, gcov)

    for suffix in sorted(config):
        best = best_coverage(summaries, suffix)
        shown = "missing" if best is None else f"{best[0]:6.2f}% of {best[1]} lines"
        print(f"{suffix:55s} {shown}  (min {config[suffix]:.0f}%)")

    failures = evaluate(config, summaries)
    for failure in failures:
        print(f"COVERAGE FAIL {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
