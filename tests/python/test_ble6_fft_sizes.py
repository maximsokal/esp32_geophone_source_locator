"""Exercise each supported Nordic FFT length with identical channel input."""
import math
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ble6_research_replay as replay


@pytest.mark.parametrize("size", [512, 1024, 2048])
def test_nordic_fft_sizes_recover_single_path_and_preserve_phase(size):
    if not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS 3.4.0 is unavailable")
    iq = []
    for channel in range(75):
        phase = -4 * math.pi * 1e6 * 3 * channel / 299792458
        iq.extend([math.cos(phase), math.sin(phase)])
    case = {"iq": iq, "counts": [1] * 75, "rtt": math.nan}
    work = replay.BUILD / f"fft-size-test-{size}"
    (stock, selected, phase, _), = replay.build_and_replay([case], workdir=work, nfft=size)[0]
    assert stock == pytest.approx(3, abs=0.05)
    assert selected == pytest.approx(stock, abs=1e-5)
    assert phase == pytest.approx(3, abs=0.01)


def test_unsupported_fft_size_is_rejected():
    with pytest.raises(ValueError, match="FFT size"):
        replay.build_and_replay([], nfft=256)


def test_real_wieme_nlos_larger_fft_can_select_a_worse_peak():
    source = replay.WIEME / "data/channel_map/NLOS/50_spread.csv"
    if not source.exists() or not Path("C:/ncs/v3.4.0/nrf/subsys/bluetooth/cs_de/cs_de.c").is_file():
        pytest.skip("NCS or Wieme dataset unavailable")
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    results = {}
    for size in (512, 2048):
        output, _ = replay.build_and_replay([case], replay.BUILD / f"fft-real-case-{size}", nfft=size)
        results[size] = output[0][1]
    assert results[512] == pytest.approx(3.56408, abs=.01)
    assert results[2048] == pytest.approx(9.16845, abs=.01)
    assert abs(results[2048] - case["truth_m"]) > abs(results[512] - case["truth_m"]) + 5
