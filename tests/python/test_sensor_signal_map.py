from __future__ import annotations

import copy
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import run_sensor_signal_checks as runner


def sensor_map() -> dict:
    return json.loads((ROOT / runner.MAP).read_text(encoding="utf-8"))


def test_production_map_resolves_every_contract_suite_and_declaration():
    data = sensor_map()
    tests = runner.check_map(ROOT, data)
    assert len(tests) == len(data["suites"])
    assert {row["family"] for row in data["contracts"]} == {
        "BUS", "INA", "AD", "MAIN", "REMOTE", "CS", "INPUT", "SYSTEM"}
    assert all(data["suites"][test["name"]].get("receipts") for test in tests
               if test["name"].startswith("sensor_signal_"))


@pytest.mark.parametrize("fault", ["duplicate", "unknown_suite", "unused_suite", "missing_reaction",
                                  "missing_source", "outside_source", "missing_test", "enum", "field"])
def test_map_rejects_incomplete_or_drifted_contracts(fault):
    data = sensor_map()
    if fault == "duplicate":
        data["contracts"].append(copy.deepcopy(data["contracts"][0]))
    elif fault == "unknown_suite":
        data["contracts"][0]["tests"].append("nonexistent")
    elif fault == "unused_suite":
        data["suites"]["unused"] = next(iter(data["suites"].values()))
    elif fault == "missing_reaction":
        data["contracts"][0]["reaction"] = ""
    elif fault in ("missing_source", "outside_source"):
        data["contracts"][0]["sources"] = ["missing.c" if fault == "missing_source" else "../outside.c"]
    elif fault == "missing_test":
        name = next(iter(data["suites"]))
        replacement = "nonexistent_test"
        data["suites"][replacement] = data["suites"].pop(name)
        for row in data["contracts"]:
            row["tests"] = [replacement if item == name else item for item in row["tests"]]
    else:
        guard = next(row for row in data["declarations"] if row["kind"] == ("enum" if fault == "enum" else "struct"))
        guard["members"].append("ADDED_UNMAPPED_SIGNAL")
    with pytest.raises(runner.RunnerError):
        runner.check_map(ROOT, data)


def passing_evidence():
    data = {"contracts": [{"id": "SENSOR.SAMPLE", "tests": ["sample"]}],
            "suites": {"sample": {"receipts": ["SENSOR.WORDS"]}}}
    host = {"results": [{"name": "sample", "status": "PASS", "phase": "run", "exit_code": 0,
                         "stdout": "SIGNAL_CASE SENSOR.WORDS PASS words=65536\n"}]}
    return data, host


def test_only_executed_pass_with_receipt_and_unchanged_source_can_pass():
    data, host = passing_evidence()
    report = runner.assess(data, host, 0, {"driver.c": "a"}, {"driver.c": "a"})
    assert report["status"] == "PASS"
    assert report["contracts"] == {"SENSOR.SAMPLE": "PASS"}
    assert report["hardware_hil"] == "NOT_RUN"


@pytest.mark.parametrize("fault", ["no_report", "missing", "extra", "duplicate", "build_only", "failed",
                                  "exit_code", "runner_exit", "receipt", "changed_source"])
def test_incomplete_evidence_cannot_be_reported_as_pass(fault):
    data, host = passing_evidence()
    exit_code = 0
    after = {"driver.c": "a"}
    if fault == "no_report":
        host = {}
    elif fault == "missing":
        host["results"] = []
    elif fault == "extra":
        host["results"].append({"name": "unmapped", "status": "PASS"})
    elif fault == "duplicate":
        host["results"].append(host["results"][0].copy())
    elif fault == "build_only":
        host["results"][0]["phase"] = "build"
    elif fault == "failed":
        host["results"][0]["status"] = "FAIL"
    elif fault == "exit_code":
        host["results"][0]["exit_code"] = 1
    elif fault == "runner_exit":
        exit_code = 1
    elif fault == "receipt":
        host["results"][0]["stdout"] = ""
    else:
        after["driver.c"] = "b"
    report = runner.assess(data, host, exit_code, {"driver.c": "a"}, after)
    assert report["status"] == "FAIL"
    assert report["contracts"]["SENSOR.SAMPLE"] == "FAIL"


def test_flat_declarations_ignore_comments_but_reject_unrecognized_layout():
    assert runner.declaration_members("typedef enum { A=0, /* B, */ C=-1 } result;", "enum", "result") == ["A", "C"]
    assert runner.declaration_members("typedef struct { int x[3]; bool valid; } sample;", "struct", "sample") == ["x", "valid"]
    with pytest.raises(runner.RunnerError):
        runner.declaration_members("typedef struct { struct { int x; } nested; } sample;", "struct", "sample")


def test_fingerprints_include_wrapper_fixtures_and_driver_dependencies():
    data = sensor_map()
    selected = [entry for entry in runner.check_map(ROOT, data) if entry["name"] == "sensor_signal_barometer"]
    hashes = runner.fingerprints(ROOT, data, selected)
    for path in ("tests/host/main_barometer_test.c", "components/main_barometer/main_barometer.c",
                 "components/main_barometer/include/main_barometer.h", runner.MAP):
        assert len(hashes[path]) == 64
    # Stubs can share a filename across targets and use <...> as well as "...".
    headers = {path.relative_to(ROOT).as_posix() for entry in selected
               for folder in entry.get("include_dirs", [])
               for path in (ROOT / folder).rglob("*.h")}
    assert headers <= hashes.keys()
