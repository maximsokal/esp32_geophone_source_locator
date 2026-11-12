"""Independent synthetic checks of the research replay's post-estimator policy."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ble6_research_replay as replay


def test_firmware_policy_synthetic(tmp_path):
    work = ROOT / '.test-build' / 'ble6-policy' / tmp_path.name
    replay.build_and_replay([], workdir=work)
    manifest = work / 'policy-manifest.json'
    manifest.write_text(json.dumps({'version': 1, 'tests': [{
        'name': 'ble6_replay_firmware_policy',
        'sources': ['tests/host/ble6_replay_firmware_policy_test.c',
                    'nRF54L15/src/nrf_cs_measurement_quality.c'],
        'include_dirs': [work.relative_to(ROOT).as_posix(), 'nRF54L15/include'],
        'libraries': ['m'],
    }]}), encoding='utf-8')
    result = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest', str(manifest),
                             '--build-dir', str(work / 'policy-build')], cwd=ROOT, capture_output=True,
                            text=True, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr


def test_replay_emits_firmware_sidecar_without_changing_raw_controls(tmp_path):
    work = ROOT / '.test-build' / 'ble6-policy' / tmp_path.name
    cases = []
    for nominal in (3.3, 14.0):
        iq, counts = [], []
        for channel in range(75):
            present = not 20 <= channel < 50
            angle = -4*math.pi*1e6*channel*nominal/299792458.0
            iq.extend([math.cos(angle) if present else 0, math.sin(angle) if present else 0])
            counts.append(int(present))
        cases.append({'iq': iq, 'counts': counts, 'rtt': math.nan})
    values, _ = replay.build_and_replay(cases, workdir=work)
    legacy = replay.confidence_extra(work)
    firmware = replay.firmware_results(work)
    assert len(values) == len(legacy) == len(firmware) == 2
    assert (work/'firmware.bin').read_bytes()[:4] == b'CSF1' and (work/'firmware.bin').stat().st_size == 44
    for raw, control in zip(values, legacy):
        assert len(raw) == len(control) == 4 and math.isfinite(raw[0]) and math.isfinite(raw[1])
        assert control[2] >= 600
    assert values[0][1] < 1.8
    assert firmware[0] == (-2147483648, 0, 9, 1, 0)
    distance, confidence, reason, method, valid = firmware[1]
    assert values[1][1] > 1.8
    assert valid == 1 and method == 1 and confidence <= 400 and reason == 9
    assert distance == int(values[1][1] * 1000 + 0.5) - 1800
    assert (work/'output.bin').stat().st_size == 32
    assert (work/'extra.bin').stat().st_size == 32 and (work/'peak.bin').stat().st_size == 40
    env = os.environ.copy()
    env.pop('BLE6_REPLAY_FIRMWARE', None)
    env.update({'BLE6_REPLAY_INPUT': str(work/'input.bin'), 'BLE6_REPLAY_OUTPUT': str(work/'control-output.bin'),
                'BLE6_REPLAY_EXTRA': str(work/'control-extra.bin'), 'BLE6_REPLAY_PEAK': str(work/'control-peak.bin')})
    run = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest', str(work/'manifest.json'),
                          '--build-dir', str(work/'control-build')], cwd=ROOT, env=env, capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    for name in ('output', 'extra', 'peak'):
        assert (work/f'{name}.bin').read_bytes() == (work/f'control-{name}.bin').read_bytes()
    env.pop('BLE6_REPLAY_EXTRA', None)
    env.pop('BLE6_REPLAY_PEAK', None)
    env['BLE6_REPLAY_FIRMWARE'] = str(work/'firmware-only.bin')
    env['BLE6_REPLAY_OUTPUT'] = str(work/'firmware-only-output.bin')
    run = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest', str(work/'manifest.json'),
                          '--build-dir', str(work/'firmware-only-build')], cwd=ROOT, env=env,
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert (work/'firmware-only.bin').read_bytes() == (work/'firmware.bin').read_bytes()


@pytest.mark.parametrize('payload', [
    b'BAD1', b'CSF1\0', b'CSF1' + struct.pack('<5i', 1, 100, 1, 1, 2),
    b'CSF1' + struct.pack('<5i', 1, 1001, 1, 1, 1),
    b'CSF1' + struct.pack('<5i', 1, 100, 1, 0, 1),
    b'CSF1' + struct.pack('<5i', 1, 0, 1, 1, 0),
])
def test_firmware_sidecar_decoder_rejects_corruption(tmp_path, payload):
    (tmp_path/'firmware.bin').write_bytes(payload)
    with pytest.raises(ValueError):
        replay.firmware_results(tmp_path)
