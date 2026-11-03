"""Format, native SDK mathematics and geometry checks for optional new datasets."""
import csv
import importlib.util
import math
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ble6_additional_research as extra

FIELDS = "procedure,ap,channel,i_local,q_local,i_remote,q_remote,rtt_acc_half_ns,rtt_count,quality,ifft,phase_slope,rtt,best".split(",")


def write_report(tmp_path, entries):
    path = tmp_path / "01m.csv"
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        base = dict(zip(FIELDS, [1,0,0,1,0,1,0,-40,1,0,1,1,0,1]))
        for changes in entries:
            writer.writerow(base | changes)
    return path


def test_report_slot_zero_is_not_physical_channel_zero(tmp_path):
    case, = extra.may_file(write_report(tmp_path, [{}]))
    assert case["counts"][0] == 1 and case["tones"][0][0] == 1
    assert sum(case["counts"]) == 1
    assert "raw_steps" not in case  # This is a post-aggregation report, not original PCT.


def test_zero_exported_tone_remains_observed(tmp_path):
    case, = extra.may_file(write_report(tmp_path, [{"i_local":0, "i_remote":0}]))
    assert case["counts"][0] == 1


@pytest.mark.parametrize("change", [{}, {"channel":1, "rtt_count":2}])
def test_duplicate_tones_and_inconsistent_metadata_are_rejected(tmp_path, change):
    with pytest.raises(ValueError):
        list(extra.may_file(write_report(tmp_path, [{}, change])))


def test_invalid_slot_quarantines_whole_procedure_without_guessing_channel(tmp_path):
    path = write_report(tmp_path, [{}, {"channel":6465}, {"procedure":2}])
    with pytest.raises(ValueError, match="0..74"):
        list(extra.may_file(path))
    issues = []
    cases = list(extra.may_file(path, issues))
    assert [c["procedure"] for c in cases] == [2]
    assert issues[0]["slot"] == 6465


def test_circle_geometry_reproduces_positive_half_plane_and_rejects_impossible_ranges():
    assert extra.circle_position((0,0), (6,0), 5, 5) == (3,4)
    assert extra.circle_position((0,0), (6,0), 1, 1) is None
    assert extra.circle_position((0,0), (6,0), None, 5) is None
    assert extra.circle_position((0,0), (0,0), 5, 5) is None


def test_statistics_use_correct_time_units():
    result = extra.metrics([1, -2], unit="s")
    assert result["mae_s"] == 1.5 and "mae_m" not in result


def test_actual_sdk_report_call_handles_positive_negative_and_absent_rtt(tmp_path, monkeypatch):
    import os
    sdk = Path(os.environ.get("BLE6_NCS_ROOT", "C:/ncs/v3.4.0/nrf"))
    if not (sdk / "subsys/bluetooth/cs_de/cs_de.c").exists():
        pytest.skip("NCS 3.4.0 is not installed")
    monkeypatch.setattr(extra, "BUILD", extra.ROOT / ".test-build/ble6-new-datasets" / tmp_path.name)
    tones = [[0.0]*75 for _ in range(4)]
    counts = [0]*75
    for k in range(75):
        if k+2 in (23,24,25): continue
        phase = -4*math.pi*1e6*3*k/299792458.0
        tones[0][k], tones[2][k], tones[3][k] = 1, math.cos(phase), math.sin(phase)
        counts[k] = 1
    case = {"tones":tones, "counts":counts, "acc":40, "count":1}
    positive, negative, absent = extra.report_replay([case, dict(case, acc=-40), dict(case, count=0)])
    assert abs(positive[0]-3) < .05 and abs(positive[4]-3) < .05
    assert abs(positive[2]-40*.25e-9*299792458.0) < 1e-6
    assert negative[2] == 0 and negative[7] < 500
    assert math.isnan(absent[2]) and abs(absent[4]-3) < .05


def test_optional_milan_static_positions_are_mathematically_reproducible():
    base = extra.DATA / "Bluetooth_channel_sounding_localization"
    if not base.exists(): pytest.skip("Optional Milan dataset is absent")
    root = extra.project("Bluetooth_channel_sounding_localization", "analyse_experiment_3.py").parent
    result, _ = extra.milan_static(root)
    assert result["files"] == 42 and result["rows"] == 3780
    assert result["position_reproduction_difference"]["max_m"] < .001
    # These are equivalence checks. Absolute position errors remain in the report.
    assert result["position_error"]["over_2m"] > 0


def test_optional_dynamic_geometry_conflicts_are_not_used_as_ground_truth():
    base = extra.DATA / "Bluetooth_channel_sounding_localization"
    if not base.exists(): pytest.skip("Optional Milan dataset is absent")
    root = extra.project("Bluetooth_channel_sounding_localization", "analyse_experiment_3.py").parent
    result, _ = extra.milan_dynamic(root)
    assert result["runs"] == 42
    conflicts = [r for r in result["results"] if r["status"] == "ANCHOR_CONFLICT"]
    assert conflicts
    assert all(r["proxy_usable_rows"] == 0 for r in conflicts)
