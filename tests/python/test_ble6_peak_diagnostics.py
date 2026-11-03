"""The Nordic peak trace must reproduce the real FFT-size counterexample."""
import math
import sys
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ble6_research_replay as replay
from ble6_peak_diagnostics import diagnose


def test_trace_reproduces_real_peak_switch_and_valid_profiles():
    if not (replay.WIEME / "data/channel_map/NLOS/50_spread.csv").exists():
        pytest.skip("Wieme dataset unavailable")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").exists():
        pytest.skip("NCS unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    traces = {size: diagnose(case, size) for size in (512, 2048)}
    for size, trace in traces.items():
        assert trace["nfft"] == size
        assert len(trace["profile"]) == size
        assert all(math.isfinite(x) and x >= 0 for x in trace["profile"])
        assert trace["global_index"] == max(range(size), key=lambda i: trace["profile"][i])
        assert trace["selected_index"] == trace["compensated_index"]
        assert trace["distance_m"] == pytest.approx(trace["replayed_distance_m"], abs=1e-4)
    assert traces[512]["distance_m"] == pytest.approx(3.56408, abs=.01)
    assert traces[2048]["distance_m"] == pytest.approx(9.16845, abs=.01)
    assert traces[512]["first_index"] == 12
    assert traces[512]["first_amplitude"] / traces[512]["global_amplitude"] > .4
    assert traces[2048]["first_index"] == traces[2048]["global_index"]
    profile = traces[2048]["profile"]
    assert profile[49] > profile[48] and profile[49] > profile[50]
    assert profile[49] / traces[2048]["global_amplitude"] < .4
