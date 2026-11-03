import sys
from pathlib import Path
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import ble6_research_replay as replay
from ble6_repeat_probe import first_good_per_channel


def step(channel, value, quality=0):
    return bytes([channel]) + replay.pct_payload(value, 0, quality) + replay.pct_payload(2, 0, 0)


def test_keeps_first_high_quality_pair_per_channel():
    raw = step(2, 1, 1) + step(2, 3) + step(2, 8) + step(4, 4)
    case = {"raw_steps": raw, "iq": [0.0]*150, "counts": [0]*75}
    result = first_good_per_channel(case)
    assert result["raw_steps"] == step(2, 3) + step(4, 4)
    assert result["counts"][0] == result["counts"][2] == 1
    assert sum(result["counts"]) == 2
    assert np.allclose(result["iq"][:6], [6, 0, 0, 0, 8, 0])


def test_real_repeated_tones_can_change_selected_peak():
    if not (replay.WIEME / "data/channel_map/NLOS/50_spread.csv").exists():
        pytest.skip("Wieme dataset unavailable")
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    single = first_good_per_channel(case)
    assert [n > 0 for n in case["counts"]] == [n > 0 for n in single["counts"]]
    assert sum(n > 1 for n in case["counts"]) == 45
    results, _ = replay.build_and_replay([case, single], replay.BUILD / "repeat-real-regression")
    assert results[0][1] == pytest.approx(3.56408, abs=.01)
    assert results[1][1] == pytest.approx(2.34213, abs=.01)
