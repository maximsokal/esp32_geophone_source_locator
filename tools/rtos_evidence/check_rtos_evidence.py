#!/usr/bin/env python3
"""Validate and report the existing ESP32-S3 / nRF54L15 RTOS evidence map.

Matrix schema v1 is a JSON object with ``schema_version``, top-level
``native_sim_evaluation`` and ``numeric_hard_failures`` fields, and
``scenarios``. Each scenario has case/device and
fault metadata; ``config_reference``, source/config hashes, sample rate and
channel count; ``evidence_layer`` (HOST, TARGET, HIL, or N/A); an ``oracle``
and ``fault_activated_proof``; manifest-backed ``host_tests`` entries
(``manifest`` + ``test_name``); optional Python ``contract_tests``; mutation
IDs; ``critical``; a coverage ``status``; and a HIL procedure reference.
Coverage status is not an execution result. Optional runner reports are joined
to scenarios only in the emitted run manifest and never promote HOST evidence
to TARGET/HIL. A run manifest also records Git/config hashes and tool versions.
The checker uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
from typing import Any


SCHEMA_VERSION = 1
STATUSES = {"HOST_COVERED", "HOST_PARTIAL", "BLOCKED_NO_BOARD", "N/A"}
LAYERS = {"HOST", "TARGET", "HIL", "N/A"}
MATRIX_RELATIVE = Path("tools/rtos_evidence/rtos_evidence_matrix.json")


def repo_root_default() -> Path:
    return Path(__file__).resolve().parents[2]


def read_json(path: Path, errors: list[str], label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"{label}: cannot read JSON {path}: {exc}")
        return None


def contained_file(root: Path, relative: str, label: str, errors: list[str]) -> Path | None:
    candidate = Path(relative)
    if candidate.is_absolute():
        errors.append(f"{label}: path must be repository-relative: {relative}")
        return None
    resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError:
        errors.append(f"{label}: path escapes repository: {relative}")
        return None
    if not resolved.is_file():
        errors.append(f"{label}: file not found: {relative}")
        return None
    return resolved


def mutation_index(root: Path, errors: list[str]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for path in sorted((root / "tools/mutations").glob("*.json")):
        raw = read_json(path, errors, "mutation list")
        if not isinstance(raw, list):
            errors.append(f"mutation list: expected an array: {path.relative_to(root).as_posix()}")
            continue
        for index, entry in enumerate(raw):
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
                errors.append(f"mutation list: invalid entry {path.relative_to(root).as_posix()}[{index}]")
                continue
            mutation_id = entry["id"]
            if mutation_id in result:
                errors.append(f"mutation list: duplicate id {mutation_id}")
            result[mutation_id] = entry
    return result


def normalize_manifest_path(value: str) -> str:
    return PurePosixPath(value.replace("\\", "/")).as_posix()


def mutation_targets_test(mutation: dict[str, Any], manifest: str, test_name: str) -> bool:
    tests = mutation.get("tests")
    if not isinstance(tests, list):
        return False
    manifest = normalize_manifest_path(manifest)
    for item in tests:
        if not isinstance(item, dict):
            continue
        if (
            normalize_manifest_path(str(item.get("manifest", ""))) == manifest
            and item.get("only") == test_name
        ):
            return True
    return False


def validate_matrix(matrix_path: Path, repo_root: Path | None = None) -> tuple[dict[str, Any] | None, list[str]]:
    root = (repo_root or repo_root_default()).resolve()
    errors: list[str] = []
    raw = read_json(matrix_path, errors, "matrix")
    if not isinstance(raw, dict):
        return None, errors or ["matrix: expected an object"]
    if raw.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"matrix: schema_version must be {SCHEMA_VERSION}")
    hard_failures = raw.get("numeric_hard_failures")
    required_hard_failures = {
        "unaccounted dropped frame", "deadlock", "unexpected reset",
        "session corruption", "false COMPLETE", "missed deadline",
    }
    if (
        not isinstance(hard_failures, list)
        or any(not isinstance(item, str) for item in hard_failures)
        or not required_hard_failures.issubset(set(hard_failures))
    ):
        errors.append("matrix: numeric_hard_failures must include all six required hard-fail conditions")
    if not isinstance(raw.get("fatal_fault_policy"), str) or not raw["fatal_fault_policy"].strip():
        errors.append("matrix: fatal_fault_policy must be non-empty")
    if not isinstance(raw.get("semantics_boundary"), str) or not raw["semantics_boundary"].strip():
        errors.append("matrix: semantics_boundary must be non-empty")
    native = raw.get("native_sim_evaluation")
    if not isinstance(native, dict) or not isinstance(native.get("status"), str) or native.get("status") not in {"RUNNABLE", "NOT RUNNABLE"}:
        errors.append("matrix: native_sim_evaluation.status must be RUNNABLE or NOT RUNNABLE")
    elif native["status"] == "NOT RUNNABLE" and not str(native.get("reason", "")).strip():
        errors.append("matrix: NOT RUNNABLE native_sim_evaluation needs a reason")

    scenarios = raw.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        errors.append("matrix: scenarios must be a non-empty array")
        return raw, errors

    mutations = mutation_index(root, errors)
    seen: set[str] = set()
    required = {
        "case_id", "device", "kind", "fault_point", "seeded_schedule", "src_sha",
        "config_reference", "config_sha256", "sample_rate_hz", "channel_count",
        "evidence_layer", "expected_detection", "recovery_invariant", "fail_safe_policy",
        "oracle", "fault_activated_proof", "host_tests", "mutations", "critical",
        "status", "blocked_reason", "hil_procedure_ref",
    }
    for index, row in enumerate(scenarios):
        prefix = f"scenario[{index}]"
        if not isinstance(row, dict):
            errors.append(f"{prefix}: expected an object")
            continue
        missing = sorted(required - row.keys())
        if missing:
            errors.append(f"{prefix}: missing fields: {', '.join(missing)}")
        case_id = row.get("case_id")
        if not isinstance(case_id, str) or not case_id.strip():
            errors.append(f"{prefix}: case_id must be a non-empty string")
        elif case_id in seen:
            errors.append(f"{prefix}: duplicate case_id {case_id}")
        else:
            seen.add(case_id)
        device = row.get("device")
        if not isinstance(device, str) or device not in {"ESP32-S3", "nRF54L15", "CROSS_DEVICE"}:
            errors.append(f"{prefix}: invalid device {device!r}")

        layer = row.get("evidence_layer")
        status = row.get("status")
        if not isinstance(layer, str) or layer not in LAYERS:
            errors.append(f"{prefix}: invalid evidence_layer {layer!r}")
        if isinstance(layer, str) and layer in {"TARGET", "HIL"} and status in {"PASS", "HOST_COVERED"}:
            errors.append(f"{prefix}: {layer} evidence cannot claim {status}")
        if not isinstance(status, str) or status not in STATUSES:
            errors.append(f"{prefix}: invalid status {status!r}")
        if status == "HOST_COVERED" and layer != "HOST":
            errors.append(f"{prefix}: HOST_COVERED requires evidence_layer HOST")
        blocked_reason = row.get("blocked_reason")
        if status == "BLOCKED_NO_BOARD" and (not isinstance(blocked_reason, str) or not blocked_reason.strip()):
            errors.append(f"{prefix}: BLOCKED_NO_BOARD requires blocked_reason")
        if status != "BLOCKED_NO_BOARD" and row.get("blocked_reason") not in (None, ""):
            errors.append(f"{prefix}: blocked_reason is only valid for BLOCKED_NO_BOARD")
        if not isinstance(row.get("critical"), bool):
            errors.append(f"{prefix}: critical must be boolean")
        for field in ("oracle", "fault_activated_proof", "expected_detection", "recovery_invariant", "fail_safe_policy"):
            if not isinstance(row.get(field), str) or not row[field].strip():
                errors.append(f"{prefix}: {field} must be non-empty")

        host_tests = row.get("host_tests")
        if not isinstance(host_tests, list):
            errors.append(f"{prefix}: host_tests must be an array")
            host_tests = []
        for test in host_tests:
            if not isinstance(test, dict) or not isinstance(test.get("manifest"), str) or not isinstance(test.get("test_name"), str):
                errors.append(f"{prefix}: host_tests entries need manifest and test_name")
                continue
            manifest_path = contained_file(root, test["manifest"], prefix + " host test", errors)
            if manifest_path is None:
                continue
            manifest_data = read_json(manifest_path, errors, prefix + " host manifest")
            names = {
                item.get("name") for item in manifest_data.get("tests", [])
                if isinstance(item, dict)
            } if isinstance(manifest_data, dict) and isinstance(manifest_data.get("tests"), list) else set()
            if test["test_name"] not in names:
                errors.append(f"{prefix}: host test not found: {test['manifest']}#{test['test_name']}")

        contract_tests = row.get("contract_tests", [])
        if not isinstance(contract_tests, list):
            errors.append(f"{prefix}: contract_tests must be an array")
            contract_tests = []
        for item in contract_tests:
            if not isinstance(item, str):
                errors.append(f"{prefix}: contract_tests entries must be strings")
                continue
            path = item.split("::", 1)[0]
            contained_file(root, path, prefix + " contract test", errors)

        mutation_ids = row.get("mutations")
        if not isinstance(mutation_ids, list):
            errors.append(f"{prefix}: mutations must be an array")
            mutation_ids = []
        row_mutations: list[dict[str, Any]] = []
        for mutation_id in mutation_ids:
            if not isinstance(mutation_id, str) or mutation_id not in mutations:
                errors.append(f"{prefix}: mutation id not found: {mutation_id}")
            else:
                row_mutations.append(mutations[mutation_id])
        if row.get("critical") is True:
            for test in host_tests:
                if not isinstance(test, dict) or not isinstance(test.get("manifest"), str) or not isinstance(test.get("test_name"), str):
                    continue
                if not any(mutation_targets_test(item, test["manifest"], test["test_name"]) for item in row_mutations):
                    errors.append(
                        f"{prefix}: critical host test lacks a referenced killer mutation: "
                        f"{test['manifest']}#{test['test_name']}"
                    )
        if status == "HOST_COVERED" and (not host_tests or not mutation_ids):
            errors.append(f"{prefix}: HOST_COVERED requires at least one host test and mutation")

        procedure = row.get("hil_procedure_ref")
        if not isinstance(procedure, str) or not procedure.strip():
            errors.append(f"{prefix}: hil_procedure_ref must be non-empty")
        else:
            contained_file(root, procedure.split("#", 1)[0], prefix + " HIL procedure", errors)

    return raw, errors


def report_data(path: Path | None, errors: list[str], label: str) -> dict[str, Any] | None:
    if path is None:
        return None
    raw = read_json(path, errors, label)
    if not isinstance(raw, dict) or not isinstance(raw.get("results"), list):
        errors.append(f"{label}: expected runner JSON object with a results array")
        return None
    if label == "host test report":
        if not isinstance(raw.get("manifest"), str):
            errors.append(f"{label}: expected the manifest path written by tools/run_host_tests.py")
        for result in raw["results"]:
            if not isinstance(result, dict) or not isinstance(result.get("name"), str) or not isinstance(result.get("status"), str):
                errors.append(f"{label}: each result needs name and status")
    elif label == "mutation report":
        for result in raw["results"]:
            if not isinstance(result, dict) or not isinstance(result.get("id"), str) or not isinstance(result.get("status"), str):
                errors.append(f"{label}: each result needs id and status")
    return raw


def attach_results(
    matrix: dict[str, Any],
    root: Path,
    host_report: dict[str, Any] | None,
    mutation_report: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    host_manifest = None
    host_results: dict[str, Any] = {}
    if host_report is not None:
        manifest = host_report.get("manifest")
        if isinstance(manifest, str):
            try:
                host_manifest = normalize_manifest_path(Path(manifest).resolve().relative_to(root.resolve()).as_posix())
            except ValueError:
                host_manifest = normalize_manifest_path(manifest)
        host_results = {
            item["name"]: item for item in host_report["results"]
            if isinstance(item, dict) and isinstance(item.get("name"), str)
        }
    mutation_results = {
        item["id"]: item for item in (mutation_report or {}).get("results", [])
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    attached: list[dict[str, Any]] = []
    for row in matrix.get("scenarios", []):
        if not isinstance(row, dict):
            continue
        observed_host = []
        for test in row.get("host_tests", []):
            if not isinstance(test, dict):
                continue
            same_manifest = host_manifest == normalize_manifest_path(test.get("manifest", ""))
            result = host_results.get(test.get("test_name")) if same_manifest else None
            if result is not None:
                observed_host.append({"manifest": test["manifest"], "test_name": test["test_name"], "status": result.get("status")})
        observed_mutations = [
            {"mutation_id": mutation_id, "status": mutation_results[mutation_id].get("status")}
            for mutation_id in row.get("mutations", []) if mutation_id in mutation_results
        ]
        if observed_host or observed_mutations:
            attached.append({
                "case_id": row.get("case_id"),
                "host_tests": observed_host,
                "mutations": observed_mutations,
            })
    return attached


def git_value(root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *args], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=0.35, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def command_version(name: str, args: tuple[str, ...]) -> str:
    executable = shutil.which(name)
    if executable is None:
        return "not found"
    try:
        result = subprocess.run(
            [executable, *args], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=0.25, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    if result.returncode:
        return "unavailable"
    lines = (result.stdout.strip() or result.stderr.strip()).splitlines()
    return lines[0][:160] if lines else "unknown"


def file_sha256(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()
    except OSError:
        return None


def kconfig_int(paths: list[Path], key: str) -> tuple[int | None, str | None]:
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        match = re.search(rf"(?m)^\s*CONFIG_{re.escape(key)}=(\d+)\s*$", text)
        if match:
            return int(match.group(1)), path.as_posix()
    return None, None


def macro_int(path: Path, macro: str) -> tuple[int | None, str | None]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None, None
    match = re.search(rf"(?m)^\s*#\s*define\s+{re.escape(macro)}\s+(\d+)", text)
    return (int(match.group(1)), path.as_posix()) if match else (None, None)


def emit_run_manifest(
    root: Path,
    matrix: dict[str, Any],
    matrix_path: Path,
    output_path: Path,
    host_report_path: Path | None,
    mutation_report_path: Path | None,
    host_report: dict[str, Any] | None,
    mutation_report: dict[str, Any] | None,
) -> None:
    if output_path.resolve() == matrix_path.resolve():
        raise ValueError("--run-manifest cannot overwrite the matrix")
    sha = git_value(root, "rev-parse", "HEAD")
    dirty_state = git_value(root, "status", "--porcelain", "--untracked-files=normal")
    if sha is None or dirty_state is None:
        raise ValueError("could not read Git SHA and dirty state")
    sample_rate, sample_source = kconfig_int(
        [root / "sdkconfig", root / "sdkconfig.defaults"],
        "SEISMIC_AD7606_SAMPLE_RATE_HZ",
    )
    channel_count, channel_source = macro_int(
        root / "components/seismic_buffer/include/seismic_buffer.h",
        "SEISMIC_CHANNEL_COUNT",
    )
    config_paths = [
        root / "sdkconfig",
        root / "sdkconfig.defaults",
        root / "nRF54L15/zephyr/prj.conf",
        root / "build/zephyr/.config",
    ]
    profile_root = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".ncs-workspaces/esp32_geophone_source_locator/nrf54l15-production-v3.4.0"
    config_paths.append(profile_root / "zephyr/zephyr/.config")
    config_hashes = [
        {"path": path.as_posix(), "sha256": file_sha256(path)}
        for path in config_paths
    ]
    ncs_script = (root / "nRF54L15/ncs.ps1").read_text(encoding="utf-8", errors="replace")
    ncs_match = re.search(r'\$NcsVersion\s*=\s*"([^"]+)"', ncs_script)
    attached_results = attach_results(root=root, matrix=matrix, host_report=host_report, mutation_report=mutation_report)
    scenario_provenance = []
    for row in matrix.get("scenarios", []):
        device = row.get("device")
        relevant_hashes = [
            item for item in config_hashes
            if device == "CROSS_DEVICE"
            or (device == "ESP32-S3" and Path(item["path"]).name in {"sdkconfig", "sdkconfig.defaults"})
            or (device == "nRF54L15" and (Path(item["path"]).name == "prj.conf" or Path(item["path"]).name == ".config"))
        ]
        scenario_provenance.append({
            "case_id": row.get("case_id"),
            "src_sha": sha,
            "config_reference": row.get("config_reference"),
            "config_sha256": relevant_hashes,
        })
    data = {
        "schema_version": 1,
        "git_sha": sha,
        "dirty": bool(dirty_state),
        "tool_versions": {
            "python": platform.python_version(),
            "git": command_version("git", ("--version",)),
            "ast-grep": command_version("ast-grep", ("--version",)),
            "west": command_version("west", ("--version",)),
            "ncs": ncs_match.group(1) if ncs_match else None,
        },
        "config_sha256": config_hashes,
        "scenario_provenance": scenario_provenance,
        "signal_configuration": {
            "ESP32-S3": {
                "sample_rate_hz": sample_rate,
                "sample_rate_source": sample_source,
                "channel_count": channel_count,
                "channel_count_source": channel_source,
            },
            "nRF54L15": {
                "sample_rate_hz": None,
                "channel_count": None,
                "note": "event-driven sensor snapshots; no seismic ADC sample/channel configuration",
            },
        },
        "evidence_layer": "HOST",
        "host_test_report": str(host_report_path) if host_report_path else None,
        "mutation_report": str(mutation_report_path) if mutation_report_path else None,
        "observed_scenario_results": attached_results,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def status_counts(matrix: dict[str, Any]) -> dict[str, dict[str, int]]:
    devices = {"ESP32-S3": {}, "nRF54L15": {}, "CROSS_DEVICE": {}}
    for row in matrix.get("scenarios", []):
        if not isinstance(row, dict):
            continue
        device = row.get("device")
        if device not in devices:
            continue
        status = str(row.get("status"))
        devices[device][status] = devices[device].get(status, 0) + 1
    return devices


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_default())
    parser.add_argument("--matrix", type=Path, help="matrix path; defaults to tools/rtos_evidence/rtos_evidence_matrix.json")
    parser.add_argument("--host-test-report", type=Path, help="optional tools/run_host_tests.py host-test-report.json")
    parser.add_argument("--mutation-report", type=Path, help="optional tools/run_mutation_tests.py --json-report output")
    parser.add_argument("--run-manifest", type=Path, help="write a local run manifest; output is not committed")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = args.repo_root.resolve()
    matrix_path = args.matrix or (root / MATRIX_RELATIVE)
    if not matrix_path.is_absolute():
        matrix_path = root / matrix_path
    matrix, errors = validate_matrix(matrix_path, root)
    host_report = report_data(args.host_test_report, errors, "host test report")
    mutation_report = report_data(args.mutation_report, errors, "mutation report")
    if errors:
        for error in errors:
            print(f"RTOS EVIDENCE ERROR: {error}", file=sys.stderr)
        return 1
    assert matrix is not None
    if args.run_manifest:
        output_path = args.run_manifest if args.run_manifest.is_absolute() else root / args.run_manifest
        try:
            emit_run_manifest(root, matrix, matrix_path, output_path, args.host_test_report, args.mutation_report, host_report, mutation_report)
        except (OSError, ValueError) as exc:
            print(f"RTOS EVIDENCE ERROR: cannot emit run manifest: {exc}", file=sys.stderr)
            return 1
        print(f"Run manifest: {output_path}")
    if host_report is not None or mutation_report is not None:
        print(
            "Attached reports: "
            f"host={len(host_report.get('results', [])) if host_report else 0}, "
            f"mutations={len(mutation_report.get('results', [])) if mutation_report else 0}"
        )
    counts = status_counts(matrix)
    total = len(matrix["scenarios"])
    print(f"RTOS evidence matrix: {total} scenarios")
    for device, statuses in counts.items():
        summary = ", ".join(f"{key}={value}" for key, value in sorted(statuses.items())) or "no rows"
        print(f"{device}: {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
