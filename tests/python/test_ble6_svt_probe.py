"""Tests for an exploratory SVT completion, not the unavailable IEEE implementation."""
import sys
from pathlib import Path
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ble6_svt_probe import complete
from ble6_svt_benchmark import combined
import ble6_research_replay as replay


def test_known_single_path_recovers_gap_without_altering_observed_tones():
    x = np.exp(-2j * np.pi * np.arange(75) * .03)
    mask = np.ones(75, dtype=bool)
    mask[25:35] = False
    measured = np.where(mask, x, 0)
    result = complete(measured, mask)
    assert np.array_equal(result[mask], measured[mask])
    assert np.linalg.norm(result[~mask] - x[~mask]) < np.linalg.norm(x[~mask])


def test_missing_observations_are_rejected():
    with pytest.raises(ValueError, match="observed"):
        complete(np.zeros(75, dtype=complex), np.zeros(75, dtype=bool))


def test_real_nlos_iq_can_be_completed_and_sent_to_nordic():
    source = replay.WIEME / "data/channel_map/NLOS/50_spread.csv"
    if not source.exists() or not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS or Wieme dataset unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    iq = combined(case)
    observed = np.asarray(case["counts"]) > 0
    filled = complete(iq, observed)
    assert np.array_equal(filled[observed], iq[observed])
    channels = np.flatnonzero(observed)
    counts = observed.astype(int)
    counts[channels[0]:channels[-1]+1] = 1
    candidate = dict(case, iq=np.column_stack((filled.real, filled.imag)).ravel().tolist(),
                     counts=counts.tolist(), raw_steps=b"")
    estimates, _ = replay.build_and_replay([case, candidate], replay.BUILD / "svt-real-case")
    assert all(np.isfinite(value[1]) for value in estimates)
