#!/usr/bin/env python3
"""Check the Wi-Fi / HTTP / multiplexer / SD / Channel Sounding signal map and
run its native tests without hardware.

    python tools/run_peripheral_signal_checks.py              # map check + all suites
    python tools/run_peripheral_signal_checks.py --check-map  # map consistency only

The map is docs/peripheral-signals.json. It lists contracts (inputs, outputs,
reaction, sources, test suites), the suites with the manifest that defines
them, the PERIPHERAL_CASE ids each new suite must print, the defects found
by those tests and the observations that were not changed.
"""

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
MAP = "docs/peripheral-signals.json"
FAMILIES = {"WIFI", "HTTP", "MUX", "SD", "CS"}
CASE_LINE = re.compile(r"^PERIPHERAL_CASE (\S+) PASS$", re.M)
STATUS_VALUES = {"used", "internal", "reduced", "dropped", "unavailable", "derived"}


def declaration_members(source: str, kind: str, name: str) -> list[str]:
    """Members of a flat public C struct/enum typedef (nested ones fail loudly)."""
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


def load_map(root: Path) -> dict:
    return json.loads((root / MAP).read_text(encoding="utf-8"))


def check_map(root: Path, data: dict) -> list[dict]:
    """Validate the map against the tree. Returns the manifest entries to run."""
    if data.get("version") != 1:
        raise RunnerError("peripheral map needs version 1")
    for key in ("contracts", "suites", "findings", "observations", "declarations", "capabilities"):
        if not data.get(key):
            raise RunnerError(f"peripheral map has no {key}")

    contracts = data["contracts"]
    ids = [row["id"] for row in contracts]
    if len(ids) != len(set(ids)):
        raise RunnerError("duplicate contract id")
    suites = data["suites"]
    used: set[str] = set()
    for row in contracts:
        for key in ("family", "inputs", "outputs", "reaction", "sources", "tests"):
            if not row.get(key):
                raise RunnerError(f"{row['id']}: missing {key}")
        if row["family"] not in FAMILIES or not row["id"].startswith(row["family"] + "."):
            raise RunnerError(f"{row['id']}: bad family {row['family']}")
        for source in row["sources"]:
            if not resolve_repo_path(root, source, "signal source").is_file():
                raise RunnerError(f"{row['id']}: missing source {source}")
        for name in row["tests"]:
            if name not in suites:
                raise RunnerError(f"{row['id']}: unknown suite {name}")
            used.add(name)
    if used != set(suites):
        raise RunnerError(f"suites not used by any contract: {sorted(set(suites) - used)}")
    covered = {row["family"] for row in contracts}
    if covered != FAMILIES:
        raise RunnerError(f"families without a contract: {sorted(FAMILIES - covered)}")

    manifests: dict[Path, list[dict]] = {}
    selected: list[dict] = []
    for name, spec in suites.items():
        path = resolve_repo_path(root, spec["manifest"], "test manifest")
        if path not in manifests:
            load_manifest(path, root)
            manifests[path] = json.loads(path.read_text(encoding="utf-8"))["tests"]
        matches = [entry for entry in manifests[path] if entry["name"] == name]
        if len(matches) != 1:
            raise RunnerError(f"missing/non-unique suite {name} in {spec['manifest']}")
        selected.append(matches[0])
        cases = spec.get("cases", [])
        if len(cases) != len(set(cases)):
            raise RunnerError(f"{name}: duplicate case id")
        if name.startswith("peripheral_signal_") and not cases:
            raise RunnerError(f"{name}: a new signal suite must declare its PERIPHERAL_CASE ids")

    # Every case a new suite prints must be declared, and the other way round.
    for name, spec in suites.items():
        if not name.startswith("peripheral_signal_"):
            continue
        entry = next(item for item in selected if item["name"] == name)
        test_source = [s for s in entry["sources"] if s.endswith("_test.c")]
        if len(test_source) != 1:
            raise RunnerError(f"{name}: expected exactly one *_test.c source")
        text = resolve_repo_path(root, test_source[0], "signal test").read_text(encoding="utf-8")
        printed = set(re.findall(r'CASE_PASS\("([^"]+)"\)', text))
        if printed != set(spec["cases"]):
            raise RunnerError(
                f"{name}: declared cases differ from the test source: "
                f"missing {sorted(set(spec['cases']) - printed)}, undeclared {sorted(printed - set(spec['cases']))}")

    all_cases = {case for spec in suites.values() for case in spec.get("cases", [])}
    for finding in data["findings"]:
        for key in ("id", "family", "summary", "files", "test", "case", "status"):
            if not finding.get(key):
                raise RunnerError(f"finding {finding.get('id')}: missing {key}")
        if finding["test"] not in suites or finding["case"] not in suites[finding["test"]].get("cases", []):
            raise RunnerError(f"finding {finding['id']}: test/case is not a declared signal case")
        for file in finding["files"]:
            if not resolve_repo_path(root, file, "finding file").is_file():
                raise RunnerError(f"finding {finding['id']}: missing file {file}")
    for observation in data["observations"]:
        if not observation.get("id") or not observation.get("summary") or observation.get("family") not in FAMILIES:
            raise RunnerError(f"bad observation {observation}")
    if not all_cases:
        raise RunnerError("no signal cases declared")

    cs = data["capabilities"].get("CS", {})
    if not cs.get("rows") or not cs.get("configuration"):
        raise RunnerError("CS capability table is empty")
    for row in cs["rows"]:
        for key in ("signal", "stack", "firmware", "esp", "use", "status"):
            if not row.get(key):
                raise RunnerError(f"CS capability row {row.get('signal')}: missing {key}")
        if row["status"] not in STATUS_VALUES:
            raise RunnerError(f"CS capability row {row['signal']}: bad status {row['status']}")

    seen = set()
    for guard in data["declarations"]:
        key = (guard["path"], guard["type"])
        if key in seen or guard["contract"] not in ids:
            raise RunnerError(f"bad or duplicate declaration guard: {key}")
        seen.add(key)
        source = resolve_repo_path(root, guard["path"], "declaration").read_text(encoding="utf-8")
        actual = declaration_members(source, guard["kind"], guard["type"])
        if actual != guard["members"]:
            raise RunnerError(
                f"{guard['type']} changed: map has {guard['members']}, source has {actual}; "
                f"update the map and the tests of {guard['contract']}")
    return selected


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(root: Path, entries: list[dict]) -> dict[str, str]:
    files: set[str] = set()
    for entry in entries:
        files.update(entry.get("sources", []))
    return {name: sha256(resolve_repo_path(root, name, "digest")) for name in sorted(files)}


def head(root: Path) -> str:
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def run(root: Path, data: dict, entries: list[dict], build_dir: Path, sanitizers: str) -> dict:
    out = root / ".test-build" / "peripheral-signals"
    out.mkdir(parents=True, exist_ok=True)
    manifest = out / "manifest.json"
    manifest.write_text(json.dumps({"version": 1, "tests": entries}, indent=2), encoding="utf-8")
    before = tree_digest(root, entries)
    command = [sys.executable, str(root / "tools" / "run_host_tests.py"), "--manifest", str(manifest),
               "--build-dir", str(build_dir), "--keep-going"]
    if sanitizers != "none":
        command += ["--sanitizers", sanitizers]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
    (out / "host-tests.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    report_path = build_dir / "host-test-report.json"
    if not report_path.is_file():
        raise RunnerError(f"no host test report; see {out / 'host-tests.log'}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    after = tree_digest(root, entries)
    results = {item["name"]: item for item in report["results"]}

    problems: list[str] = []
    if before != after:
        problems.append("a source changed while the tests were running")
    for name, spec in data["suites"].items():
        item = results.get(name)
        if item is None or item["status"] != "PASS" or item.get("phase") != "run":
            problems.append(f"{name}: not PASS ({None if item is None else (item['status'], item.get('phase'))})")
            continue
        passed = set(CASE_LINE.findall(item.get("stdout", "")))
        missing = set(spec.get("cases", [])) - passed
        if missing:
            problems.append(f"{name}: no PASS line for {sorted(missing)}")
    verification = {
        "head": head(root),
        "map": MAP,
        "suites": {name: results[name]["status"] for name in data["suites"] if name in results},
        "contracts": len(data["contracts"]),
        "sanitizers": sanitizers,
        "source_sha256": after,
        "problems": problems,
        "hardware_hil": "NOT_RUN",
    }
    (out / "verification.json").write_text(json.dumps(verification, indent=2), encoding="utf-8")
    if problems:
        raise RunnerError("peripheral signal checks failed:\n  " + "\n  ".join(problems))
    return verification


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-map", action="store_true", help="validate the map only; run no test")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".test-build" / "peripheral-signals" / "build")
    parser.add_argument("--sanitizers", choices=("none", "address"), default="none")
    args = parser.parse_args()
    try:
        data = load_map(ROOT)
        entries = check_map(ROOT, data)
        print(f"map OK: {len(data['contracts'])} contracts, {len(data['suites'])} suites, "
              f"{len(data['findings'])} findings, {len(data['observations'])} observations")
        if args.check_map:
            return 0
        verification = run(ROOT, data, entries, args.build_dir, args.sanitizers)
        print(f"PERIPHERAL_SIGNALS PASS: {len(verification['suites'])} suites; hardware/HIL NOT_RUN")
        return 0
    except RunnerError as error:
        print(f"PERIPHERAL_SIGNALS FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
