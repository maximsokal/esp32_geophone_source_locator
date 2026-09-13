#!/usr/bin/env python3
"""Build one offline acceptance report from a saved ESP32 UART log.

The runner never opens serial ports, flashes firmware, resets hardware, or
controls power. It combines the independent boot/input, storage/recorder, and
priority analyzers while preserving the distinction between machine-observable
evidence and manual physical attestation.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import importlib.util
import json
from pathlib import Path
import sys
from typing import Any


TOOLS_DIR = Path(__file__).resolve().parent
EXPECTED_KEYS = (1, 2, 3, 4, 5)


class AcceptanceError(RuntimeError):
    pass


@dataclass(frozen=True)
class IssueResult:
    issue: str
    machine_status: str
    evidence: tuple[str, ...]
    manual_required: tuple[str, ...]


@dataclass(frozen=True)
class Report:
    machine_status: str
    max6: IssueResult
    max7: IssueResult
    max10: IssueResult
    max11: IssueResult
    storage_baseline_loaded: bool


def _load_module(filename: str, module_name: str):
    path = TOOLS_DIR / filename
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise AcceptanceError(f"cannot load analyzer: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def _aggregate_status(statuses: tuple[str, ...]) -> str:
    if "FAIL" in statuses:
        return "FAIL"
    if "BLOCKED" in statuses:
        return "BLOCKED"
    return "PASS"


def _max6_result(boot: Any) -> IssueResult:
    key_sets = {
        "raw": tuple(boot.raw_keys),
        "button": tuple(boot.button_keys),
        "post": tuple(boot.posted_keys),
        "dispatch": tuple(boot.dispatched_keys),
        "ui-result": tuple(boot.ui_result_keys),
    }
    expected = set(EXPECTED_KEYS)
    missing_stages = [
        f"{name} missing={','.join(map(str, sorted(expected - set(values))))}"
        for name, values in key_sets.items()
        if set(values) != expected
    ]

    failures: list[str] = []
    if boot.stale_drop_count:
        failures.append(f"stale keypad drops={boot.stale_drop_count}")
    if boot.safe_shutdown_count:
        failures.append(f"unexpected safe shutdowns={boot.safe_shutdown_count}")
    if boot.failure_marker_count:
        failures.append(f"panic/watchdog/brownout/assert markers={boot.failure_marker_count}")

    status = "FAIL" if failures else ("BLOCKED" if missing_stages else "PASS")
    evidence = tuple(
        [
            "full software key chain observed for 1=LEFT,2=UP,3=DOWN,4=RIGHT,5=OK"
            if not missing_stages
            else "; ".join(missing_stages),
            f"max dispatch age={boot.max_dispatch_age_ms} ms",
        ]
        + failures
    )
    return IssueResult(
        issue="MAX-6",
        machine_status=status,
        evidence=evidence,
        manual_required=(
            "confirm visible TFT response for the physical key actions",
            "confirm intentional post-startup GPIO21 long press performs ordered safe shutdown",
        ),
    )


def _max7_result(boot: Any) -> IssueResult:
    evidence = (
        f"startup_total={boot.startup_ms} ms",
        f"max recovery window={boot.max_recovery_window_ms} ms",
        f"stale keypad drops={boot.stale_drop_count}",
        "background recovery/cache ordering checked by boot analyzer",
    )
    return IssueResult(
        issue="MAX-7",
        machine_status=boot.status,
        evidence=evidence + tuple(f"missing: {item}" for item in boot.missing)
        + tuple(f"violation: {item}" for item in boot.violations),
        manual_required=(
            "confirm physical TFT remains visibly responsive while background storage work runs",
        ),
    )


def _max11_result(priority: Any, require_inherited: bool) -> IssueResult:
    evidence = (
        f"priority samples={priority.sample_count}",
        f"targeted pre-release samples={priority.storage_pre_release_sample_count}",
        f"inherited samples={priority.inherited_sample_count}",
        f"max current priority={priority.max_current_priority}",
        f"expected base priority={priority.expected_base_priority}",
    )
    manual = (
        "confirm capture was taken under the storage-contention scenario intended to reproduce the historical observation",
    )
    if not require_inherited:
        manual += (
            "if closing by 'no longer reproduces', document capture duration/workload rather than treating one base-priority sample as proof",
        )
    return IssueResult(
        issue="MAX-11",
        machine_status=priority.status,
        evidence=evidence + tuple(f"missing: {item}" for item in priority.missing)
        + tuple(f"violation: {item}" for item in priority.violations),
        manual_required=manual,
    )


def _max10_blocked(reason: str) -> IssueResult:
    return IssueResult(
        issue="MAX-10",
        machine_status="BLOCKED",
        evidence=(reason,),
        manual_required=(
            "measure and document the real SD/SDSPI/sample-rate/channel baseline before performance acceptance",
        ),
    )


def _max10_result(storage: Any, baseline: Any) -> IssueResult:
    evidence = (
        f"baseline measured_at={baseline.measured_at}",
        f"baseline sd_card={baseline.sd_card}",
        f"baseline sdspi_hz={baseline.sdspi_hz}",
        f"baseline sample_rate_hz={baseline.sample_rate_hz}",
        f"baseline channels={baseline.channels}",
        f"baseline firmware_commit={baseline.firmware_commit}",
        f"max mutex wait={storage.max_mutex_wait_us} us",
        f"max TFT quiesce={storage.max_tft_quiesce_us} us",
        f"max TFT resume={storage.max_tft_resume_us} us",
        f"max SPI end elapsed={storage.max_spi_end_elapsed_us} us",
        f"max recorder window={storage.max_recorder_window_us} us",
        f"max recorder fwrite={storage.max_recorder_fwrite_us} us",
        f"max recorder operation={storage.max_recorder_operation_us} us",
        f"max dropped frames={storage.max_dropped_frames}",
    )
    return IssueResult(
        issue="MAX-10",
        machine_status=storage.status,
        evidence=evidence + tuple(f"missing: {item}" for item in storage.missing)
        + tuple(f"violation: {item}" for item in storage.violations),
        manual_required=(
            "confirm the baseline JSON was derived from the real board/card/configuration, not parser fixtures",
        ),
    )


def analyze_report(
    content: str,
    *,
    storage_baseline_path: Path | None = None,
    require_inherited: bool = False,
    minimum_priority_samples: int = 1,
) -> Report:
    boot_module = _load_module(
        "analyze_boot_responsiveness.py", "acceptance_boot_analyzer"
    )
    priority_module = _load_module(
        "analyze_web_status_priority.py", "acceptance_priority_analyzer"
    )

    boot = boot_module.analyze(content)
    priority = priority_module.analyze(
        content,
        minimum_samples=minimum_priority_samples,
        require_inherited=require_inherited,
        require_storage_pre_release=True,
    )

    max6 = _max6_result(boot)
    max7 = _max7_result(boot)
    max11 = _max11_result(priority, require_inherited)

    baseline_loaded = False
    if storage_baseline_path is None:
        max10 = _max10_blocked("no measured storage baseline profile supplied")
    else:
        baseline_module = _load_module(
            "analyze_storage_baseline.py", "acceptance_baseline_analyzer"
        )
        storage_module = _load_module(
            "analyze_storage_spi.py", "acceptance_storage_analyzer"
        )
        try:
            baseline = baseline_module.load_baseline(storage_baseline_path)
        except Exception as exc:  # normalized into a report-level configuration error
            raise AcceptanceError(f"invalid storage baseline: {exc}") from exc
        limits = baseline.limits_us
        storage = storage_module.analyze(
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
        max10 = _max10_result(storage, baseline)
        baseline_loaded = True

    machine_status = _aggregate_status(
        (
            max6.machine_status,
            max7.machine_status,
            max10.machine_status,
            max11.machine_status,
        )
    )
    return Report(
        machine_status=machine_status,
        max6=max6,
        max7=max7,
        max10=max10,
        max11=max11,
        storage_baseline_loaded=baseline_loaded,
    )


def print_report(report: Report) -> None:
    print(f"HARDWARE_ACCEPTANCE_MACHINE_RESULT: {report.machine_status}")
    for issue in (report.max6, report.max7, report.max10, report.max11):
        print(f"\n===== {issue.issue} machine={issue.machine_status} =====")
        for item in issue.evidence:
            print(f"EVIDENCE: {item}")
        for item in issue.manual_required:
            print(f"MANUAL_REQUIRED: {item}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="saved passive UART log")
    parser.add_argument(
        "--storage-baseline",
        type=Path,
        help="measured storage baseline JSON; MAX-10 is BLOCKED when omitted",
    )
    parser.add_argument(
        "--require-inherited",
        action="store_true",
        help="require at least one inherited web_status_cache sample for MAX-11",
    )
    parser.add_argument("--minimum-priority-samples", type=int, default=1)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.minimum_priority_samples < 1:
        print(
            "HARDWARE_ACCEPTANCE_MACHINE_RESULT: FAIL: "
            "--minimum-priority-samples must be >= 1",
            file=sys.stderr,
        )
        return 1

    try:
        content = args.log.read_text(encoding="utf-8", errors="replace")
        report = analyze_report(
            content,
            storage_baseline_path=args.storage_baseline,
            require_inherited=args.require_inherited,
            minimum_priority_samples=args.minimum_priority_samples,
        )
    except (OSError, AcceptanceError) as exc:
        print(f"HARDWARE_ACCEPTANCE_MACHINE_RESULT: FAIL: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(asdict(report), indent=2, sort_keys=True))
    else:
        print_report(report)

    if report.machine_status == "PASS":
        return 0
    if report.machine_status == "BLOCKED":
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
