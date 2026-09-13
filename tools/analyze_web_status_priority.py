#!/usr/bin/env python3
"""Analyze captured web_status_cache priority evidence offline.

The analyzer accepts the existing periodic RUNTIME_TASK snapshots and the
stronger optional STORAGE_PRIORITY pre-release marker. It never opens serial
ports or changes task priorities.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import sys


@dataclass(frozen=True)
class PrioritySample:
    source: str
    current: int
    base: int
    inherited: int
    elapsed_us: int | None = None


@dataclass(frozen=True)
class Analysis:
    status: str
    missing: tuple[str, ...]
    violations: tuple[str, ...]
    sample_count: int
    runtime_sample_count: int
    storage_pre_release_sample_count: int
    inherited_sample_count: int
    max_current_priority: int | None
    expected_base_priority: int


RUNTIME_RE = re.compile(
    r"RUNTIME_TASK\s+rank=\d+\s+name=web_status_cach(?:e)?\s+core=-?\d+\s+"
    r"priority_current=(?P<current>\d+)\s+priority_base=(?P<base>\d+)\s+"
    r"inherited=(?P<inherited>[01])",
    re.I,
)
STORAGE_RE = re.compile(
    r"STORAGE_PRIORITY\s+stage=pre-release\s+owner=web_status_cach(?:e)?\s+"
    r"priority_current=(?P<current>\d+)\s+priority_base=(?P<base>\d+)\s+"
    r"inherited=(?P<inherited>[01])(?:\s+elapsed_us=(?P<elapsed>\d+))?",
    re.I,
)


def parse_samples(content: str) -> list[PrioritySample]:
    samples: list[PrioritySample] = []
    for match in RUNTIME_RE.finditer(content):
        samples.append(
            PrioritySample(
                source="runtime-top-n",
                current=int(match.group("current")),
                base=int(match.group("base")),
                inherited=int(match.group("inherited")),
            )
        )
    for match in STORAGE_RE.finditer(content):
        elapsed = match.group("elapsed")
        samples.append(
            PrioritySample(
                source="storage-pre-release",
                current=int(match.group("current")),
                base=int(match.group("base")),
                inherited=int(match.group("inherited")),
                elapsed_us=int(elapsed) if elapsed is not None else None,
            )
        )
    return samples


def analyze(
    content: str,
    *,
    expected_base_priority: int = 1,
    minimum_samples: int = 1,
    require_inherited: bool = False,
    require_storage_pre_release: bool = False,
) -> Analysis:
    samples = parse_samples(content)
    runtime_samples = [sample for sample in samples if sample.source == "runtime-top-n"]
    storage_samples = [
        sample for sample in samples if sample.source == "storage-pre-release"
    ]

    missing: list[str] = []
    violations: list[str] = []

    if len(samples) < minimum_samples:
        missing.append(f"web_status_cache priority samples ({len(samples)}/{minimum_samples})")
    if require_storage_pre_release and not storage_samples:
        missing.append("STORAGE_PRIORITY pre-release web_status_cache sample")

    inherited_count = 0
    for sample in samples:
        if sample.base != expected_base_priority:
            violations.append(
                f"{sample.source} priority_base={sample.base} expected={expected_base_priority}"
            )
        expected_inherited = int(sample.current > sample.base)
        if sample.inherited != expected_inherited:
            violations.append(
                f"{sample.source} inherited={sample.inherited} inconsistent with "
                f"current={sample.current} base={sample.base}"
            )
        if sample.inherited:
            inherited_count += 1

    if require_inherited and samples and inherited_count == 0:
        missing.append("inherited web_status_cache priority sample")

    max_current = max((sample.current for sample in samples), default=None)
    status = "FAIL" if violations else ("BLOCKED" if missing else "PASS")
    return Analysis(
        status=status,
        missing=tuple(missing),
        violations=tuple(violations),
        sample_count=len(samples),
        runtime_sample_count=len(runtime_samples),
        storage_pre_release_sample_count=len(storage_samples),
        inherited_sample_count=inherited_count,
        max_current_priority=max_current,
        expected_base_priority=expected_base_priority,
    )


def print_report(result: Analysis) -> None:
    print(f"WEB_STATUS_PRIORITY_ANALYSIS: {result.status}")
    print(
        "samples="
        f"{result.sample_count} runtime_top_n={result.runtime_sample_count} "
        f"storage_pre_release={result.storage_pre_release_sample_count} "
        f"inherited={result.inherited_sample_count}"
    )
    print(f"max_current_priority={result.max_current_priority}")
    print(f"expected_base_priority={result.expected_base_priority}")
    for item in result.missing:
        print(f"MISSING: {item}")
    for item in result.violations:
        print(f"FAIL: {item}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="saved UART log")
    parser.add_argument("--expected-base-priority", type=int, default=1)
    parser.add_argument("--minimum-samples", type=int, default=1)
    parser.add_argument(
        "--require-inherited",
        action="store_true",
        help="BLOCKED unless at least one current>base inherited sample is captured",
    )
    parser.add_argument(
        "--require-storage-pre-release",
        action="store_true",
        help="BLOCKED unless the targeted storage pre-release marker is captured",
    )
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.minimum_samples < 1:
        print("WEB_STATUS_PRIORITY_ANALYSIS: FAIL: --minimum-samples must be >= 1", file=sys.stderr)
        return 1
    if args.expected_base_priority < 0:
        print("WEB_STATUS_PRIORITY_ANALYSIS: FAIL: expected base priority must be >= 0", file=sys.stderr)
        return 1

    try:
        content = args.log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"WEB_STATUS_PRIORITY_ANALYSIS: FAIL: cannot read {args.log}: {exc}", file=sys.stderr)
        return 1

    result = analyze(
        content,
        expected_base_priority=args.expected_base_priority,
        minimum_samples=args.minimum_samples,
        require_inherited=args.require_inherited,
        require_storage_pre_release=args.require_storage_pre_release,
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
