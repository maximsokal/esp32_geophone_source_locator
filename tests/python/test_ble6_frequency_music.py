import math
import sys
from pathlib import Path
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ble6_frequency_music import estimate
import ble6_research_replay as replay
from ble6_svt_benchmark import combined


def test_single_path_with_channel_gap_preserves_frequency_spacing():
    channels = np.arange(75)
    z = np.exp(-4j*np.pi*1e6*channels*3/299792458.)
    mask = np.ones(75, dtype=bool)
    mask[23:26] = False
    assert estimate(z, mask, 1) == pytest.approx(3., abs=.16)


def test_insufficient_contiguous_observations_return_unavailable():
    mask = np.arange(75) % 2 == 0
    assert math.isnan(estimate(np.ones(75), mask, 1))


def test_rejects_nonfinite_observed_tones():
    z = np.ones(75, dtype=complex)
    z[10] = complex(float("nan"), 0)
    with pytest.raises(ValueError, match="finite"):
        estimate(z, np.ones(75, dtype=bool), 1)


def test_real_nlos_spectrum_can_lock_to_the_wrong_path():
    if not (replay.WIEME / "data/channel_map/NLOS/50_spread.csv").exists():
        pytest.skip("Wieme dataset unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    result = estimate(combined(case), np.asarray(case["counts"]) > 0, 1)
    assert result == pytest.approx(9.3685, abs=.01)
    assert abs(result - case["truth_m"]) > 6
