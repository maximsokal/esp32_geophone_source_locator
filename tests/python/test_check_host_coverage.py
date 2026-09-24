"""MAX-247: the host coverage gate's parsing and decisions."""

import importlib.util
import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_host_coverage", REPO_ROOT / "tools" / "check_host_coverage.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)

GCOV_OUTPUT = """File 'E:\\repo\\components\\power_control\\power_output.c'
Lines executed:100.00% of 23

File '/work/components/ad7606_acquisition/ad7606_acquisition.c'
Lines executed:84.12% of 548
Creating 'ad7606_acquisition.c.gcov'

File '/usr/include/stdio.h'
Lines executed:0.00% of 2
"""


def test_parses_gcc_and_llvm_style_paths():
    summary = gate.parse_gcov_summary(GCOV_OUTPUT)
    assert summary["E:/repo/components/power_control/power_output.c"] == (100.0, 23)
    assert summary["/work/components/ad7606_acquisition/ad7606_acquisition.c"] == (84.12, 548)
    assert len(summary) == 3


def test_best_binary_wins_and_suffix_must_match_whole_path_tail():
    low = {"/a/components/x/driver.c": (40.0, 100)}
    high = {"/b/components/x/driver.c": (90.0, 100)}
    other = {"/b/components/y/driver.c": (100.0, 100)}
    assert gate.best_coverage([low, high, other], "components/x/driver.c") == (90.0, 100)
    assert gate.best_coverage([low], "components/z/driver.c") is None


def test_evaluate_reports_low_and_missing_files():
    summaries = [{"/r/components/a.c": (80.0, 10), "/r/components/b.c": (95.0, 10)}]
    failures = gate.evaluate({"components/a.c": 85, "components/b.c": 90, "components/c.c": 50}, summaries)
    assert failures == [
        "components/a.c: 80.00% < required 85.00%",
        "components/c.c: no coverage data (not compiled into any host test?)",
    ]
    assert gate.evaluate({"components/b.c": 95}, summaries) == []


def test_gate_config_lists_existing_hardware_facing_files():
    config = json.loads((REPO_ROOT / "tests" / "host" / "coverage-gate.json").read_text(encoding="utf-8"))
    minimums = config["minimum_line_percent"]
    assert minimums
    for path, minimum in minimums.items():
        assert (REPO_ROOT / path).is_file(), path
        assert 0 < minimum <= 100
    for required in (
        "components/power_control/power_output.c",
        "components/ad7606_acquisition/ad7606_acquisition.c",
        "components/ad7606_driver/ad7606_driver.c",
        "components/ina3221_monitor/ina3221_driver.c",
        "components/sensor_bus/sensor_bus.c",
    ):
        assert required in minimums
