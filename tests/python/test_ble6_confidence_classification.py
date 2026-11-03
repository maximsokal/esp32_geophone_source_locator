"""The confidence of a distance estimate, checked against the public BLE Channel Sounding datasets (nRF54L15/tests/ble6).

Every procedure of a sample of OCMTEC, Wieme and OWL goes through the unchanged NCS 3.4.0 `cs_de.c`, the production PCT collector and the
production confidence code (`nrf_cs_measurement_confidence_with_peak` / `_reason`); the truth is the dataset's own reference distance.
The datasets are not tracked: the tests skip when they, or NCS 3.4.0, are missing.

What is checked (bench 2026-11-03, P0 "classification of RTT=0, insufficient confirmation and over-confidence"):
  - the reason code the firmware logs (`why=`) is what the rules say, for every procedure;
  - a sharp IFFT peak lifts the confidence of an answer without a usable RTT, and only that; nothing else moves;
  - without RTT and without a sharp peak the answer stays below the 500 production gate;
  - the answers accepted that way meet the accuracy criterion (line of sight, 1-5 m, constant offset removed: median <= 0.3 m, P95 <= 1.0 m),
    and the share of accepted answers that are badly wrong is small and stated, not hidden.
"""
import hashlib
import importlib.util
import itertools
import math
import os
import statistics
from collections import Counter
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ble6_research_replay", ROOT / "tools/ble6_research_replay.py")
research = importlib.util.module_from_spec(spec)
spec.loader.exec_module(research)

GATE = 500
SHARP = 10.0
RTT_LIMIT_M = 7.5
REASONS = {1: "ok", 2: "no_estimate", 3: "bad_input", 4: "tones_low", 5: "sharp_peak", 6: "rtt_missing", 7: "rtt_disagrees", 8: "rtt_only"}


def _need_data():
    sdk = Path(os.environ.get("BLE6_NCS_ROOT", "C:/ncs/v3.4.0/nrf"))
    if not (sdk / "subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS 3.4.0 is unavailable")
    for path in (research.WIEME / "mapping.csv", research.OWL / "owl-indoor.parquet", research.DATA / "cs_de_gaps_data_v1"):
        if not path.exists():
            pytest.skip(f"research dataset is unavailable: {path.name}")
    try:
        import pyarrow  # noqa: F401
    except ImportError:
        pytest.skip("pyarrow is unavailable")


def _reserved(case):
    return int(hashlib.sha256(("ble6-review-v1/" + case["dataset"] + "/" + case["file"]).encode()).hexdigest()[:8], 16) % 5 == 0


def _line_of_sight(case):
    parts = case["file"].split("/")
    return "LOS" in parts and "NLOS" not in parts


def _expected_reason(good, pbr, rtt_ok, best, rtt, sharp):
    """The rules of nrf_cs_measurement_confidence_reason(), written again from the documentation in the header."""
    if (pbr and not math.isfinite(best)) or (rtt_ok and not math.isfinite(rtt)):
        return "bad_input"
    if not pbr and not rtt_ok:
        return "no_estimate"
    if not pbr:
        return "rtt_only"
    if good < 15:
        return "tones_low"
    lifted = math.isfinite(sharp) and sharp > SHARP and (not rtt_ok or abs(best - rtt) <= RTT_LIMIT_M)
    if lifted and _plain(good, rtt_ok, best, rtt) < 600:
        return "sharp_peak"
    if not rtt_ok:
        return "rtt_missing"
    if abs(best - rtt) > 2.5:
        return "rtt_disagrees"
    return "ok"


def _plain(good, rtt_ok, best, rtt):
    """nrf_cs_measurement_confidence() for a PBR answer with enough tones."""
    extra = min(1.0, (good - 15) / 30.0)
    factor = 0.6 + 0.4 * extra
    if not rtt_ok:
        factor *= 0.4
    else:
        excess = abs(best - rtt) - 2.5
        if excess > 0:
            factor *= math.exp(-0.5 * (excess / 2.5) ** 2)
    return min(1000, int(factor * 1000 + 0.5))


@pytest.fixture(scope="module")
def replay():
    _need_data()
    cases = list(itertools.islice(research.ocm_cases(), 0, None, 4))
    cases += list(itertools.islice(research.wieme_cases(), 0, None, 6))
    for name in ("owl-indoor.parquet", "owl-outdoor.parquet"):
        cases.extend(research.owl_cases(research.OWL / name, 200))
    workdir = research.BUILD / "test-confidence-classification"
    results, _ = research.build_and_replay(cases, workdir=workdir)
    extra = research.confidence_extra(workdir)
    assert len(results) == len(extra) == len(cases) > 3000
    rows = []
    for case, result, tail in zip(cases, results, extra):
        good = sum(count > 0 for count in case["counts"])
        rows.append({
            "case": case, "good": good, "best": result[1], "rtt": case["rtt"], "rtt_ok": math.isfinite(case["rtt"]) and case["rtt"] > 0,
            "pbr": math.isfinite(result[0]) or math.isfinite(result[2]), "sharp": tail[0], "plain": int(tail[1]), "peak": int(tail[2]),
            "reason": REASONS[int(tail[3])], "truth": case["truth_m"],
        })
    return rows


def test_the_logged_reason_is_what_the_rules_say_for_every_procedure(replay):
    mismatches = [(r["case"]["dataset"], r["reason"], _expected_reason(r["good"], r["pbr"], r["rtt_ok"], r["best"], r["rtt"], r["sharp"]))
                  for r in replay if r["reason"] != _expected_reason(r["good"], r["pbr"], r["rtt_ok"], r["best"], r["rtt"], r["sharp"])]
    assert mismatches == [], mismatches[:5]
    assert {r["reason"] for r in replay} >= {"ok", "sharp_peak", "rtt_missing", "rtt_disagrees"}


def test_only_a_sharp_peak_with_enough_tones_and_a_tolerable_rtt_moves_the_confidence(replay):
    for r in replay:
        assert r["peak"] >= r["plain"]                                   # a floor, never a cap
        if r["peak"] != r["plain"]:
            assert r["pbr"] and r["good"] >= 15 and r["sharp"] > SHARP
            assert not r["rtt_ok"] or abs(r["best"] - r["rtt"]) <= RTT_LIMIT_M
            assert r["peak"] == 600                                      # NRF_CS_SHARP_PEAK_CONFIDENCE
        else:
            assert not (r["pbr"] and r["good"] >= 15 and r["sharp"] > SHARP and r["plain"] < 600
                        and (not r["rtt_ok"] or abs(r["best"] - r["rtt"]) <= RTT_LIMIT_M))


def test_without_rtt_and_without_a_sharp_peak_the_answer_stays_below_the_production_gate(replay):
    """Wieme records carry no RTT at all (the single-method cap of 0.4 applies to every one)."""
    wieme = [r for r in replay if r["case"]["dataset"] == "Wieme" and r["pbr"] and r["good"] >= 15]
    assert len(wieme) > 1500
    assert all(r["plain"] < GATE for r in wieme)                         # before: not one of them was usable
    sharp = [r for r in wieme if r["sharp"] > SHARP]
    unsharp = [r for r in wieme if not r["sharp"] > SHARP]
    assert all(r["peak"] >= GATE for r in sharp) and all(r["peak"] < GATE for r in unsharp)
    assert 0.25 < len(sharp) / len(wieme) < 0.60, len(sharp) / len(wieme)  # the availability the gate costs (44% on the full set)


def test_rtt_that_disagrees_with_a_sharp_answer_no_longer_hides_it_but_a_wild_rtt_still_does(replay):
    owl = [r for r in replay if r["case"]["dataset"] == "OWL" and r["rtt_ok"] and r["pbr"] and r["good"] >= 15]
    far = [r for r in owl if abs(r["best"] - r["rtt"]) > RTT_LIMIT_M]
    near_disagree = [r for r in owl if 2.5 < abs(r["best"] - r["rtt"]) <= RTT_LIMIT_M]
    assert len(near_disagree) > 100 and len(far) > 100                   # OWL's RTT is very often far off the truth
    assert all(r["peak"] == r["plain"] for r in far)                     # beyond 7.5 m the plain rule decides
    assert any(r["peak"] > r["plain"] for r in near_disagree)


def test_accepted_answers_meet_the_accuracy_criterion_on_line_of_sight_1_to_5_m(replay):
    accepted = [r for r in replay if r["case"]["dataset"] == "Wieme" and r["peak"] >= GATE and math.isfinite(r["best"])]
    development = [r for r in accepted if not _reserved(r["case"])]
    offset = statistics.median(r["best"] - r["truth"] for r in development)     # the constant offset of the board: calibrated, then removed
    reserved = sorted(abs(r["best"] - r["truth"] - offset) for r in accepted
                      if _reserved(r["case"]) and _line_of_sight(r["case"]) and 1.0 <= r["truth"] <= 5.0)
    assert len(reserved) >= 60
    p95 = reserved[int(0.95 * len(reserved))]
    assert statistics.median(reserved) <= 0.30, statistics.median(reserved)
    assert p95 <= 1.0, p95


def test_the_share_of_accepted_answers_that_are_badly_wrong_is_small_and_known(replay):
    """Over-confidence: a sharp peak is not proof of a true first path. Stated, not hidden: it bounds what the scheduler's own checks must catch."""
    accepted = [r for r in replay if r["case"]["dataset"] == "Wieme" and r["peak"] >= GATE and math.isfinite(r["best"])]
    offset = statistics.median(r["best"] - r["truth"] for r in accepted if not _reserved(r["case"]))
    wrong = [r for r in accepted if abs(r["best"] - r["truth"] - offset) > 2.0]
    assert len(accepted) > 1000
    assert len(wrong) / len(accepted) < 0.12, len(wrong) / len(accepted)
    # The accepted set is far better than the whole set: that is what the lift buys.
    everything = [r for r in replay if r["case"]["dataset"] == "Wieme" and math.isfinite(r["best"])]
    wrong_everything = [r for r in everything if abs(r["best"] - r["truth"] - offset) > 2.0]
    assert len(wrong) / len(accepted) < len(wrong_everything) / len(everything)


def test_ocmtec_records_with_rtt_are_unchanged_by_the_lift_except_where_the_rtt_is_missing_or_wild(replay):
    ocm = [r for r in replay if r["case"]["dataset"] == "OCMTEC"]
    moved = [r for r in ocm if r["peak"] != r["plain"]]
    assert Counter(r["reason"] for r in moved) == {"sharp_peak": len(moved)}
    assert len(moved) < 0.02 * len(ocm)                                  # almost all of OCMTEC was accepted before and still is
