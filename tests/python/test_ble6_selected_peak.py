"""Associate spectral quality with the path actually selected by the SDK."""
import importlib.util
import math
import struct
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('research',ROOT/'tools/ble6_research_replay.py')
research = importlib.util.module_from_spec(spec)
spec.loader.exec_module(research)


@pytest.fixture(autouse=True)
def installed_sdk():
    import os
    sdk = Path(os.environ.get('BLE6_NCS_ROOT','C:/ncs/v3.4.0/nrf'))
    if not (sdk/'subsys/bluetooth/cs_de/cs_de.c').is_file():
        pytest.skip('NCS 3.4.0 unavailable')


def test_earlier_selected_path_does_not_inherit_later_stronger_peak_quality():
    iq = []
    for channel in range(75):
        a = -4*math.pi*1e6*channel/299792458
        iq.extend([0.6*math.cos(a*3)+math.cos(a*14),
                   0.6*math.sin(a*3)+math.sin(a*14)])
    case = {'iq':iq,'counts':[1]*75,'rtt':math.nan}
    work = research.BUILD/'test-selected-peak'
    results,_ = research.build_and_replay([case],workdir=work)
    selected,maximum,index,ratio,confidence = next(struct.iter_unpack('<5f',(work/'peak.bin').read_bytes()))
    assert abs(results[0][1]-3) < 0.3
    assert index < 20
    assert 0.4 < selected/maximum < 0.8
    assert ratio < research.confidence_extra(work)[0][0]
    assert confidence <= research.confidence_extra(work)[0][2]


def test_real_coherent_wrong_path_remains_sharp_after_selected_peak_check():
    if not research.WIEME.exists():
        pytest.skip('optional Wieme IQ dataset unavailable')
    case = next(c for c in research.wieme_cases()
        if c['file']=='orientation/45angle/NLOS/0-135_M-M.csv' and c['row']==0)
    work = research.BUILD/'test-selected-wrong-path'
    results,_ = research.build_and_replay([case],workdir=work)
    *_,ratio,confidence = next(struct.iter_unpack('<5f',(work/'peak.bin').read_bytes()))
    assert abs(results[0][1]-case['truth_m']) > 2
    assert research.confidence_extra(work)[0][2]>=500
    assert ratio>10 and confidence>=500
