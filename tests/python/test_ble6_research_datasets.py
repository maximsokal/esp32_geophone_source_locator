"""Import and physical-reference checks for the optional BLE research datasets."""
import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ble6_research_replay", ROOT / "tools/ble6_research_replay.py")
research = importlib.util.module_from_spec(spec)
spec.loader.exec_module(research)


def test_paired_phase_reversals_preserve_the_three_metre_response(native_sdk):
    import math
    # Opposite oscillator phase changes cancel within each paired exchange.
    # Averaging each device first cancels the entire signal instead.
    raw = bytearray()
    counts = [0] * 75
    for channel in range(2, 77):
        if channel in (23, 24, 25):
            continue
        angle = -4.0 * math.pi * 1e6 * 3.0 * (channel - 2) / 299792458.0
        re, im = round(1024 * math.cos(angle)), round(1024 * math.sin(angle))
        for sign in (1, -1):
            raw.extend(bytes([channel]) + research.pct_payload(sign * 1024, 0, 0)
                       + research.pct_payload(sign * re, sign * im, 0))
        counts[channel - 2] = 2
    case = {"iq": [0.0] * 150, "counts": counts, "rtt": math.nan, "raw_steps": bytes(raw)}
    (_, selected, _, _), = research.build_and_replay([case], workdir=research.BUILD / "test-paired-phase")[0]
    assert abs(selected - 3.0) < 0.05, selected


def test_coherent_products_reduce_real_wieme_error_tail_without_distance_offsets(native_sdk):
    import json
    import math
    if not research.WIEME.exists():
        pytest.skip("Optional Wieme data is absent")
    cases = list(research.wieme_cases())
    inputs = []
    for case in cases:
        # The imported IQ is the independent stock product of separate means.
        inputs.extend((dict(case, raw_steps=b""), case))
    results, _ = research.build_and_replay(inputs, workdir=research.BUILD / "test-coherent-wieme")
    groups = {name: [[], []] for name in ("all", "LOS", "NLOS", "outdoor")}
    for case, before, after in zip(cases, results[::2], results[1::2]):
        if not all(math.isfinite(x) and x >= 0 for x in (before[1], after[1])):
            continue
        parts = case["file"].split("/")
        category = "LOS" if "LOS" in parts else "NLOS" if "NLOS" in parts else "outdoor"
        for group in (groups["all"], groups[category]):
            group[0].append(abs(before[1] - case["truth_m"]))
            group[1].append(abs(after[1] - case["truth_m"]))
    def metrics(errors):
        return {"n": len(errors), "mae_m": sum(errors) / len(errors),
                "rmse_m": math.sqrt(sum(e*e for e in errors) / len(errors)),
                "p95_m": research.percentile(errors, 0.95), "max_m": max(errors),
                "over_1m": sum(e > 1.0 for e in errors), "over_2m": sum(e > 2.0 for e in errors)}
    report = {name: {"before": metrics(pair[0]), "after": metrics(pair[1])} for name, pair in groups.items()}
    (research.BUILD / "test-coherent-wieme/metrics.json").write_text(json.dumps(report, indent=2))
    assert report["all"]["before"]["n"] > 21000
    for category in groups:
        old, new = report[category]["before"], report[category]["after"]
        for metric in ("mae_m", "rmse_m", "p95_m", "max_m", "over_2m"):
            assert new[metric] < old[metric], (category, metric, old, new)


@pytest.fixture
def native_sdk():
    import os
    sdk = Path(os.environ.get("BLE6_NCS_ROOT", "C:/ncs/v3.4.0/nrf"))
    if not (sdk / "subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS 3.4.0 is unavailable")


def test_wieme_import_averages_each_side_before_multiplication_and_filters_quality():
    line = "0;0;2;0;2;0;2;0,0;0;2;0;4;0;4;0,1;0;2;0;100;0;100;0,0;0;3;0;0;2;0;3"
    iq, counts = research.wieme_row(line)
    assert counts[:2] == [2, 1]
    assert iq[:4] == [9.0, 0.0, -6.0, 0.0]


def test_production_pct_collector_matches_repeated_signed_tones_and_quality_filter(native_sdk):
    import math
    line = "0;0;2;0;2;0;2;0,0;0;2;0;4;0;4;0,1;0;2;0;100;0;100;0,0;0;3;0;0;2;0;3"
    iq, counts = research.wieme_row(line)
    case = {"iq": iq, "counts": counts, "rtt": math.nan, "raw_steps": research.wieme_raw_steps(line)}
    research.build_and_replay([case], workdir=research.BUILD / "test-pct-mean")


def test_wieme_rejects_invalid_step():
    with pytest.raises(ValueError):
        research.wieme_row("0;0;2;0;1;2;3")


def test_raw_pct_replay_detects_corrupt_combined_iq_reference(native_sdk):
    import math
    # Signed 12-bit I=-2048, Q=2047; remote I=-1, Q=1.
    raw = bytes([2, 0, 0, 0xf8, 0x7f, 0, 0, 0xff, 0x1f, 0, 0])
    case = {"iq": [0.0] * 150, "counts": [1] + [0] * 74,
            "raw_steps": raw, "rtt": math.nan}
    # Deliberately wrong IQ must fail inside the native collector comparison.
    with pytest.raises(RuntimeError, match=r"src\[i\] - buf\[i\]"):
        research.build_and_replay([case], workdir=research.BUILD / "test-pct-corrupt")
    case["iq"][:2] = [1.0, -4095.0]
    research.build_and_replay([case], workdir=research.BUILD / "test-pct-signed")


def test_xu_nearest_reference_respects_500ms_and_nan():
    assert research.nearest_reference([1.0, 2.0], [5.0, 7.0], 1.5) == 5.0
    assert research.nearest_reference([1.0, 2.1], [5.0, 7.0], 1.55) is None


def test_ignore_entire_external_data_tree():
    import subprocess
    result = subprocess.run(["git", "check-ignore", "-q", "--no-index", "nRF54L15/tests/ble6/arbitrary/nested/file.csv"], cwd=ROOT)
    assert result.returncode == 0

def test_wieme_real_5m_pair_must_not_report_high_confidence_for_large_error():
    if not research.WIEME.exists():
        pytest.skip("Optional Wieme dataset is not installed under nRF54L15/tests/ble6")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not __import__("os").environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = next(c for c in research.wieme_cases() if c["file"] == "distance/LOS/5_meter.csv" and c["row"] == 0)
    results, _ = research.build_and_replay([case], workdir=research.BUILD / "test-wieme")
    _, filled, _, confidence = results[0]
    assert confidence < 500 or abs(filled - case["truth_m"]) <= 1.0, (
        f"truth={case['truth_m']:.3f} m, estimate={filled:.3f} m, "
        f"confidence={confidence:.0f}, no RTT in supplied Wieme data"
    )


def test_waves_mode2_decoding_and_truncation():
    # 12-bit -1 / +1, quality HIGH, channel 2, one tone plus extension.
    data = bytes([2, 2, 9, 0, 0xff, 0x1f, 0x00, 0, 0, 0, 0, 0x12])
    assert research.waves_steps(data) == [(2, -1, 1, 0)]
    with pytest.raises(ValueError):
        research.waves_steps(data[:-1])


def test_native_replay_matches_nordic_phase_fallback_when_ifft_is_unavailable():
    import math
    import os
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not os.environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = {"iq": [0.0] * 150, "counts": [1] * 15 + [0] * 60, "rtt": math.nan}
    (raw, selected, phase, confidence), = research.build_and_replay([case], workdir=research.BUILD / "test-fallback")[0]
    assert math.isnan(raw) and phase == 0.0 and selected == phase and confidence < 500


def test_waves_paired_capture_import_has_no_fabricated_truth():
    if not research.DATA.exists():
        pytest.skip("Optional research dataset directory is absent")
    cases = list(research.waves_cases())
    assert len(cases) > 1 and all(case["truth_m"] is None for case in cases)
    assert all(len(case["iq"]) == 150 and sum(case["counts"]) >= 15 for case in cases)


def test_owl_first_sample_matches_reported_anchor_geometry():
    pytest.importorskip("pyarrow")
    path = research.OWL / "owl-indoor.parquet"
    if not path.is_file():
        pytest.skip("Optional OWL indoor data is absent")
    import math
    case = next(research.owl_cases(path, stride=200))
    truth = math.hypot(-4.1220 - (-1.9510), -7.2614 - (-3.6483))
    assert abs(case["truth_m"] - truth) < 0.001
    assert len(case["iq"]) == 150 and sum(case["counts"]) >= 15


def test_xu_timestamp_join_uses_paired_files_and_500ms():
    if not research.XU.exists():
        pytest.skip("Optional Xu dataset is absent")
    stats = research.xu_statistics()
    assert len(stats) == 21 and all(row["matched"] > 0 for row in stats)


def test_ocm_real_iq_matches_author_filled_and_stock_estimates():
    import math
    import os
    if not (research.DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1/tones.csv").is_file():
        pytest.skip("Optional OCMTEC data is absent")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not os.environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = next(c for c in research.ocm_cases() if c["file"] == "max" and c["row"] == 2712)
    (raw, filled, _, _), = research.build_and_replay([case], workdir=research.BUILD / "test-ocm")[0]
    assert math.isclose(raw, case["author_ifft"], abs_tol=0.00001)
    assert math.isclose(filled, case["author_filled"], abs_tol=0.00001)
    assert abs(raw - filled) > 2.0


def test_real_recordings_recover_from_same_frequency_mask_as_nordic_case(native_sdk):
    import json
    import math
    if not (research.DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1/tones.csv").is_file():
        pytest.skip("Optional OCMTEC data is absent")
    # Compare the same capture before/after frequency removal, avoiding its
    # documented uncalibrated device bias as a confounding variable.
    cases = [c for c in research.ocm_cases() if c["file"] in ("quiet_pre", "quiet_post")
             and sum(n > 0 for n in c["counts"]) >= 15]
    assert cases
    inputs = [s for c in cases for s in (c, research.mask_channels(c, 25, 45))]
    results, _ = research.build_and_replay(inputs, workdir=research.BUILD / "test-frequency-mask")
    rows = []
    for case, baseline, masked in zip(cases, results[::2], results[1::2]):
        assert all(math.isfinite(x) for x in (baseline[0], baseline[1], masked[0], masked[1]))
        raw_change = abs(masked[0] - baseline[0])
        fixed_change = abs(masked[1] - baseline[0])
        if sum(n > 0 for n in case["counts"]) >= 65:
            assert fixed_change < raw_change, (case["file"], case["row"], baseline, masked)
        rows.append({"segment": case["file"], "procedure": case["row"],
                     "raw_change_m": raw_change, "filled_change_m": abs(masked[1] - baseline[1])})
    raw_median = research.percentile([r["raw_change_m"] for r in rows], 0.5)
    fixed_median = research.percentile([r["filled_change_m"] for r in rows], 0.5)
    summary = {"procedures": len(rows), "raw_median_change_m": raw_median,
               "filled_median_change_m": fixed_median,
               "nonimproved": [r for r in rows if r["filled_change_m"] >= r["raw_change_m"]]}
    (research.BUILD / "test-frequency-mask/summary.json").write_text(json.dumps(summary, indent=2))
    assert fixed_median < raw_median


def test_waves_real_iq_can_be_replayed_without_distance_reference():
    import math
    import os
    if not research.DATA.exists():
        pytest.skip("Optional Waves data is absent")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not os.environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = next(research.waves_cases())
    (raw, filled, phase, _), = research.build_and_replay([case], workdir=research.BUILD / "test-waves")[0]
    assert case["truth_m"] is None
    assert all(math.isfinite(v) for v in (raw, filled, phase))


def test_ocm_clamped_zero_rtt_is_not_a_valid_cross_check():
    import math
    import os
    if not (research.DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1/tones.csv").is_file():
        pytest.skip("Optional OCMTEC data is absent")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not os.environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = next(c for c in research.ocm_cases() if c["file"] == "quiet_pre" and c["row"] == 362)
    assert math.isnan(case["rtt"])
    (_, filled, _, confidence), = research.build_and_replay([case], workdir=research.BUILD / "test-zero-rtt")[0]
    assert math.isfinite(filled) and confidence < 500


def test_native_replay_rejects_zero_rtt_as_cross_check():
    import math
    import os
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file() and not os.environ.get("BLE6_NCS_ROOT"):
        pytest.skip("NCS 3.4.0 cs_de.c is unavailable")
    case = {"iq": [1.0, 0.0] * 75, "counts": [1] * 75, "rtt": 0.0}
    other = dict(case, rtt=math.nan)
    results, _ = research.build_and_replay([case, other], workdir=research.BUILD / "test-zero-rtt-native")
    assert results[0][3] == results[1][3] and results[0][3] < 500
