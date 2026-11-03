"""Bench cycle of 2026-11-03: the real attempts of one ranging cycle replayed through the production scheduler and policy.

The CSV holds the 102 attempts the ESP32 logged (RANGING_SAMPLE) in the cycle that ended `pair_failed:0-4`; the expected CSV holds what
the device reported for the same cycle (RANGING_PAIR). The scheduler must reproduce the device's result exactly, and it must keep the
numbers of the pair that was thrown away: the cause of the failure was not visible in the result, only in the raw attempts.
"""
import csv
import os
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/host/fixtures/bench_2026_11_03"


def _rows(name):
    with (FIXTURES / name).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(line for line in stream if not line.startswith("#")))


def _run_replay(csv_path: Path):
    scratch = ROOT / ".test-build" / f"bench-replay-{uuid.uuid4().hex[:8]}"
    scratch.mkdir(parents=True)
    out = scratch / "out.txt"
    manifest = scratch / "manifest.json"
    manifest.write_text('{"version":1,"tests":[{"name":"bench_ranging_replay","sources":["nRF54L15/src/nrf_cs_scheduler.c",'
                        '"tests/host/bench_ranging_replay.c"],"include_dirs":["nRF54L15/include"]}]}', encoding="utf-8")
    env = {**os.environ, "BENCH_RANGING_CSV": str(csv_path), "BENCH_RANGING_OUT": str(out)}
    run = subprocess.run([sys.executable, "tools/run_host_tests.py", "--manifest", str(manifest), "--build-dir", str(scratch / "build")],
                         cwd=ROOT, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    execs, pairs = {}, {}
    summary = {}
    for line in out.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if parts[0] == "SUMMARY":
            summary.update(dict(item.split("=") for item in parts[1:]))
        elif parts[0] == "SUMMARY_TINY":
            summary["tiny"] = parts[1]
        elif parts[0] == "EXEC":
            execs[int(parts[1])] = {"initiator": int(parts[2]), "reflector": int(parts[3]),
                                    **{k: int(v) for k, v in (item.split("=") for item in parts[4:])}}
        elif parts[0] == "PAIR":
            pairs[(int(parts[1]), int(parts[2]))] = {k: (v if k in ("state", "cause") else int(v))
                                                      for k, v in (item.split("=") for item in parts[3:])}
    pairs["summary"] = summary
    return execs, pairs


@pytest.fixture(scope="module")
def replay():
    return _run_replay(FIXTURES / "ranging_samples.csv")


def test_the_scheduler_reproduces_what_the_device_reported_for_the_same_attempts(replay):
    _, pairs = replay
    expected = _rows("ranging_pairs_expected.csv")
    pairs = {k: v for k, v in pairs.items() if k != "summary"}
    assert len(expected) == 6 and len(pairs) == 6
    for row in expected:
        got = pairs[(int(row["a"]), int(row["b"]))]
        assert got["state"] == row["state"], row
        for key, name in (("distance", "distance_mm"), ("confidence", "confidence"), ("valid", "valid_samples"), ("attempts", "attempts"),
                          ("rejected", "rejected"), ("spread", "spread_mm"), ("disagreement", "disagreement_mm"), ("single", "single_direction")):
            assert got[key] == int(row[name]), (row["a"], row["b"], key)


def test_every_recorded_attempt_was_consumed_by_exactly_one_execution(replay):
    execs, _ = replay
    assert len(execs) == 12 and all(e["terminal"] == 1 and e["rows_left"] == 0 for e in execs.values())
    assert sum(e["attempts"] for e in execs.values()) == 102
    # Relay order: every execution starts where the previous one ended, and the circuit ends at MAIN where it began.
    order = [execs[i] for i in range(12)]
    assert order[0]["initiator"] == 0 and order[-1]["reflector"] == 0
    assert all(order[i]["reflector"] == order[i + 1]["initiator"] for i in range(11))


def test_the_failed_pair_keeps_what_each_direction_measured_and_says_why(replay):
    _, pairs = replay
    pairs = {k: v for k, v in pairs.items() if k != "summary"}
    main_g4 = pairs[(0, 4)]
    assert main_g4["state"] == "retry_exhausted" and main_g4["distance"] == 0       # no published distance, as before
    # ... but the numbers are not lost: MAIN->G4 gave 8.8 m (three successes, only one with confidence >= 500) and G4->MAIN 6.4 m.
    assert (main_g4["diag_ab"], main_g4["n_ab"], main_g4["ok_ab"]) == (8845, 3, 1)
    assert (main_g4["diag_ba"], main_g4["n_ba"], main_g4["ok_ba"]) == (6421, 10, 9)
    # Both are beyond the 5 m cable (a true distance of at most 5 m) and they contradict each other by 2.4 m.
    assert main_g4["diag_ab"] > 5000 and main_g4["diag_ba"] > 5000 and abs(main_g4["diag_ab"] - main_g4["diag_ba"]) > 300
    # The cause is told apart from "no distance": a direction was rejected for confidence, the other was complete.
    assert main_g4["cause"] == "low_confidence"
    assert all(pairs[key]["cause"] == "none" for key in pairs if key != (0, 4))


def test_the_diagnostics_of_valid_pairs_are_the_medians_of_their_directions(replay):
    _, pairs = replay
    pairs = {k: v for k, v in pairs.items() if k != "summary"}
    # MAIN-G3: 9 + 9 samples, 3918 and 3889 mm, the published distance is their mean (the pair keeps the rejected-outlier logic).
    g3 = pairs[(0, 3)]
    assert (g3["diag_ab"], g3["diag_ba"]) == (3918, 3889) and g3["n_ab"] == g3["ok_ab"] == 9
    assert abs(g3["distance"] - (g3["diag_ab"] + g3["diag_ba"]) // 2) <= 25
    # G3-G4: a perfectly repeatable link (4099 mm in all 18 samples).
    assert (pairs[(3, 4)]["diag_ab"], pairs[(3, 4)]["diag_ba"]) == (4099, 4099)


def test_five_valid_pairs_and_one_failed_are_not_the_same_as_no_measurement(replay):
    _, pairs = replay
    summary = pairs.pop("summary")
    # The one line the log and the screens carry: how many pairs are valid and which failed, with the cause.
    assert summary["valid"] == "5/6" and summary["failed"] == "0-4:low_confidence"
    assert summary["tiny"] == "..."                          # a buffer too small for even one item says so with an ellipsis, never overruns
    valid = [key for key, pair in pairs.items() if pair["state"] == "valid"]
    failed = [key for key, pair in pairs.items() if pair["state"] != "valid"]
    assert len(valid) == 5 and failed == [(0, 4)]
    # Every valid pair of this cycle lies inside the 5 m cable bound of MAIN's links and the 10 m bound between sensors.
    for key in valid:
        bound = 5000 if 0 in key else 10000
        assert 0 < pairs[key]["distance"] <= bound, key
