#!/usr/bin/env python3
"""Check the sensor I/O map and run its native tests without accessing hardware."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from run_host_tests import RunnerError, load_manifest, resolve_repo_path


ROOT = Path(__file__).resolve().parents[1]
MAP = "docs/sensor-signals.json"


def declaration_members(source: str, kind: str, name: str) -> list[str]:
    # These are flat public C DTOs/enums; nested declarations fail explicitly.
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    pattern = rf"typedef\s+{kind}\s*(?:\w+\s*)?\{{([^{{}}]*)\}}\s*{re.escape(name)}\s*;"
    match = re.search(pattern, source)
    if match is None:
        raise RunnerError(f"cannot read flat {kind} declaration: {name}")
    if kind == "enum":
        return [part.split("=")[0].strip() for part in match[1].split(",") if part.strip()]
    members = []
    for part in match[1].split(";"):
        if not part.strip():
            continue
        field = re.search(r"\b(\w+)\s*(?:\[[^\]]*\]\s*)*$", part)
        if field is None:
            raise RunnerError(f"unsupported field in {name}: {part.strip()}")
        members.append(field[1])
    return members


def check_map(root: Path, data: dict) -> list[dict]:
    if data.get("version") != 1 or not data.get("contracts") or not data.get("suites"):
        raise RunnerError("sensor map needs version 1, contracts and suites")
    contracts = data["contracts"]
    ids = [row["id"] for row in contracts]
    if len(ids) != len(set(ids)):
        raise RunnerError("duplicate sensor contract id")
    suites = data["suites"]
    used = set()
    manifests = {}
    selected = []
    for row in contracts:
        for key in ("family", "inputs", "outputs", "reaction", "sources", "tests"):
            if not row.get(key):
                raise RunnerError(f"{row['id']}: missing {key}")
        for source in row["sources"]:
            if not resolve_repo_path(root, source, "signal source").is_file():
                raise RunnerError(f"{row['id']}: missing source {source}")
        for name in row["tests"]:
            if name not in suites:
                raise RunnerError(f"{row['id']}: unknown suite {name}")
            used.add(name)
    if used != set(suites):
        raise RunnerError(f"unmapped suites: {sorted(set(suites) - used)}")
    for name, spec in suites.items():
        path = resolve_repo_path(root, spec["manifest"], "test manifest")
        if path not in manifests:
            load_manifest(path, root)
            manifests[path] = json.loads(path.read_text(encoding="utf-8"))["tests"]
        matches = [entry for entry in manifests[path] if entry["name"] == name]
        if len(matches) != 1:
            raise RunnerError(f"missing/non-unique suite: {name} in {path}")
        selected.append(matches[0])
    guards = data.get("declarations", [])
    if not guards:
        raise RunnerError("sensor map has no declaration guards")
    seen = set()
    for guard in guards:
        key = (guard["path"], guard["type"])
        if key in seen or guard["contract"] not in ids:
            raise RunnerError(f"duplicate/unmapped declaration: {key}")
        seen.add(key)
        if guard["kind"] not in ("enum", "struct"):
            raise RunnerError(f"unsupported declaration kind: {key}")
        source = resolve_repo_path(root, guard["path"], "declaration").read_text(encoding="utf-8")
        actual = declaration_members(source, guard["kind"], guard["type"])
        if actual != guard["members"]:
            raise RunnerError(f"update signal map for {guard['type']}: expected {guard['members']}, got {actual}")
    return selected


def fingerprints(root: Path, data: dict, selected: list[dict]) -> dict[str, str]:
    """Hash mapped sources, declared include trees and local fixture .c files."""
    pending = [root / MAP, Path(__file__).resolve(), root / "tools/run_host_tests.py"]
    for spec in data["suites"].values():
        pending.append(root / spec["manifest"])
    for row in data["contracts"]:
        pending.extend(root / path for path in row["sources"])
    pending.extend(root / guard["path"] for guard in data["declarations"])
    includes = set()
    for entry in selected:
        pending.extend(root / path for path in entry["sources"])
        includes.update(root / path for path in entry.get("include_dirs", []))
    for folder in includes:
        pending.extend(path for path in folder.rglob("*") if path.suffix in (".h", ".inc"))
    hashes = {}
    while pending:
        path = pending.pop().resolve()
        relative = path.relative_to(root).as_posix()
        if relative in hashes:
            continue
        raw = path.read_bytes()
        hashes[relative] = hashlib.sha256(raw).hexdigest()
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', raw.decode("utf-8-sig"), re.M):
            for folder in (path.parent, *sorted(includes)):
                target = (folder / include).resolve()
                if target.is_file() and target.is_relative_to(root):
                    pending.append(target)
                    break
    return dict(sorted(hashes.items()))


def assess(data: dict, host: dict, exit_code: int, before: dict, after: dict) -> dict:
    results = host.get("results", [])
    names = [row["name"] for row in results]
    complete = len(names) == len(set(names)) and set(names) == set(data["suites"])
    suites = {}
    for name, spec in data["suites"].items():
        rows = [row for row in results if row["name"] == name]
        row = rows[0] if len(rows) == 1 else {}
        receipts = set(re.findall(r"^SIGNAL_CASE (\S+) PASS\b", row.get("stdout", ""), re.M))
        missing = sorted(set(spec.get("receipts", [])) - receipts)
        ok = row.get("status") == "PASS" and row.get("phase") == "run" and row.get("exit_code") == 0 and not missing
        suites[name] = {"status": "PASS" if ok else "FAIL", "missing_receipts": missing}
    changed = sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))
    valid = complete and exit_code == 0 and not changed
    contracts = {row["id"]: ("PASS" if valid and all(suites[name]["status"] == "PASS"
        for name in row["tests"]) else "FAIL") for row in data["contracts"]}
    return {
        "status": "PASS" if valid and all(value == "PASS" for value in contracts.values()) else "FAIL",
        "evidence": "native C production sources with simulated buses/RTOS; finite domains plus boundary/state tests",
        "hardware_hil": "NOT_RUN",
        "complete_suite_set": complete,
        "runner_exit_code": exit_code,
        "changed_inputs": changed,
        "contracts": contracts,
        "suites": suites,
        "source_sha256": before,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-map", action="store_true", help="validate map only; do not claim runtime coverage")
    parser.add_argument("--build-dir", type=Path, default=Path(".test-build/sensor-signals"))
    args = parser.parse_args()
    try:
        data = json.loads((ROOT / MAP).read_text(encoding="utf-8"))
        selected = check_map(ROOT, data)
        print(f"Sensor map: {len(data['contracts'])} contracts, {len(selected)} suites, "
              f"{len(data['declarations'])} declaration guards", flush=True)
        if args.check_map:
            print("SENSOR_MAP: PASS (runtime tests not run)")
            return 0
        output = (ROOT / args.build_dir).resolve()
        if not output.is_relative_to(ROOT / ".test-build") or output == ROOT / ".test-build":
            raise RunnerError("build directory must be a subdirectory of repository .test-build")
        output.mkdir(parents=True, exist_ok=True)
        manifest = output / "manifest.json"
        manifest.write_text(json.dumps({"version": 1, "tests": selected}, indent=2) + "\n", encoding="utf-8")
        host_path = output / "host-test-report.json"
        host_path.unlink(missing_ok=True)
        before = fingerprints(ROOT, data, selected)
        command = [sys.executable, str(ROOT / "tools/run_host_tests.py"), "--manifest", str(manifest),
                   "--build-dir", str(output), "--keep-going"]
        print(f"Running {len(selected)} native executables; log: {output / 'host-tests.log'}", flush=True)
        with (output / "host-tests.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=False)
        host = json.loads(host_path.read_text(encoding="utf-8")) if host_path.is_file() else {}
        report = assess(data, host, process.returncode, before, fingerprints(ROOT, data, selected))
        report["head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        report_path = output / "verification.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        for name, result in report["suites"].items():
            if result["status"] != "PASS":
                print(f"FAIL: {name}: {result}")
        if report["changed_inputs"]:
            print(f"Inputs changed while running; rerun required: {report['changed_inputs']}")
        print(f"SENSOR_SIGNALS: {report['status']} ({host.get('passed', 0)}/{len(selected)} native suites)")
        print(f"Report: {report_path}")
        return 0 if report["status"] == "PASS" else 1
    except (OSError, ValueError, KeyError, TypeError, RunnerError) as exc:
        print(f"SENSOR_SIGNALS: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
