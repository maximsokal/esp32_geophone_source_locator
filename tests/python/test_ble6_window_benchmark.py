import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ble6_window_benchmark import apply_window
import ble6_research_replay as replay
from ble6_svt_benchmark import combined


@pytest.mark.parametrize("name", ["hann", "hamming", "blackman"])
def test_window_keeps_channel_positions_and_phase(name):
    iq = np.exp(1j * np.linspace(-2, 2, 75))
    result = apply_window(iq, name)
    assert len(result) == 75
    assert abs(result[37]) == pytest.approx(1.0)
    assert np.all(np.abs(result) <= 1.0 + 1e-12)
    assert np.allclose(np.angle(result[1:-1]), np.angle(iq[1:-1]), atol=1e-12)


def test_window_rejects_wrong_shape_and_unknown_name():
    with pytest.raises(ValueError):
        apply_window(np.ones(74), "hann")
    with pytest.raises(ValueError):
        apply_window(np.ones(75), "unknown")


def test_nordic_peak_compensation_is_not_window_agnostic():
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS 3.4.0 is unavailable")
    iq = np.exp(-4j * np.pi * 1e6 * 3 * np.arange(75) / 299792458)
    signals = (iq, apply_window(iq, "hann"))
    cases = [{"iq": np.column_stack((x.real, x.imag)).ravel().tolist(),
              "counts": [1]*75, "rtt": math.nan} for x in signals]
    results, _ = replay.build_and_replay(cases, replay.BUILD / "window-clean-regression")
    assert results[0][1] == pytest.approx(3.0, abs=.01)
    assert results[1][1] == pytest.approx(1.46383, abs=.01)


def test_hamming_can_amplify_a_real_los_error():
    source = replay.WIEME / "data/orientation/height/LOS/180-90_H-M.csv"
    if not source.exists() or not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS or Wieme dataset unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "orientation/height/LOS/180-90_H-M.csv" and c["row"] == 26)
    signal = apply_window(combined(case), "hamming")
    candidate = dict(case, iq=np.column_stack((signal.real, signal.imag)).ravel().tolist(), raw_steps=b"")
    results, _ = replay.build_and_replay([case, candidate], replay.BUILD / "window-wieme-regression")
    assert case["truth_m"] == pytest.approx(2.67947, abs=.001)
    assert results[0][1] == pytest.approx(4.323, abs=.01)
    assert results[1][1] == pytest.approx(11.711, abs=.01)
