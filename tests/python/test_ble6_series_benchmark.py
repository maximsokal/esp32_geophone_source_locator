import sys
import csv
import itertools
import math
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_series_benchmark as series
import pytest


def test_windows_do_not_cross_files_runs_or_missing_procedure_numbers():
    def row(file, index, run=0):
        return {'dataset': 'test', 'file': file, 'row': index, 'run_idx': run}
    rows = [row('a', i) for i in range(9)] + [row('b', i) for i in range(11)]
    rows += [row('b', i, run=1) for i in range(11, 21)]
    rows += [row('b', i, run=1) for i in range(22, 31)]
    windows, dropped = series.windows(rows)
    assert len(windows) == 2 and dropped == 19
    assert [w[0]['row'] for w in windows] == [0, 11]


def test_actual_scheduler_cannot_identify_a_false_stable_first_cluster():
    raw = [[(8000, 900)] * 3 + [(2000, 900)] * 7,
           [(2000, 900)] * 4 + [(2000, 400)] * 6,
           [(-1, 0)] * 10]
    results = series.native_windows(raw, series.BUILD / 'test-native')
    fast, slow = results[:2]
    assert fast['target'] == 3 and fast['valid'] == 1 and fast['distance_mm'] == 8000 and fast['spread_mm'] == 33
    assert slow['target'] == 9 and slow['valid'] == 1 and slow['distance_mm'] == 2000 and slow['rejected'] == 3
    assert results[3]['valid'] == 1 and results[3]['samples'] == 4  # target9 can retain a partial direction
    assert results[4]['valid'] == results[5]['valid'] == 0


def test_source_order_changes_are_rejected():
    rows = [{'dataset': 'test', 'file': 'a', 'row': i} for i in (0, 2, 1)]
    with pytest.raises(ValueError, match='order'):
        series.windows(rows)


def test_two_complete_passes_pay_for_both_cycles():
    rows = series.native_timing(series.BUILD / 'test-timing')
    baseline = rows['9']['elapsed_ms']
    assert rows['3']['elapsed_ms'] < baseline
    assert rows['3+9']['elapsed_ms'] > baseline
    assert rows['3+6']['elapsed_ms'] > baseline
    assert rows['3+6']['procedures'] == rows['9']['procedures'] == 180
    assert rows['3+9']['procedures'] == 240
    assert rows['9']['budget_sum_ms'] <= 600000
    assert rows['3+6']['budget_sum_ms'] > 600000
    assert rows['3+9']['budget_sum_ms'] > 600000


def test_legacy_control_real_iq_first_three_can_reject_the_short_path_as_an_outlier():
    replay = series.replay
    name = 'orientation/45angle/NLOS/0-135_M-M.csv'
    path = replay.WIEME / 'data' / name
    sdk = Path(os.environ.get('BLE6_NCS_ROOT', 'C:/ncs/v3.4.0/nrf'))
    if not path.exists() or not (sdk / 'subsys/bluetooth/cs_de/cs_de.c').exists():
        pytest.skip('Optional Wieme IQ or pinned SDK unavailable')
    with (replay.WIEME / 'mapping.csv').open(encoding='utf-8-sig', newline='') as stream:
        ref = next(r for r in csv.DictReader(stream) if r['filename'].replace(chr(92), '/') == name)
    truth = math.sqrt(sum((float(ref['initiator_' + axis])-float(ref['reflector_' + axis]))**2 for axis in 'xyz'))/1000
    cases = []
    with path.open(encoding='utf-8') as stream:
        for line in itertools.islice(stream, 10):
            iq, counts = replay.wieme_row(line)
            cases.append({'iq': iq, 'counts': counts, 'rtt': math.nan, 'raw_steps': replay.wieme_raw_steps(line)})
    values, _ = replay.build_and_replay(cases, workdir=series.BUILD / 'real-prefix-iq')
    confidence = replay.confidence_extra(series.BUILD / 'real-prefix-iq')
    assert len(values) == 10 and all(c[2] >= 500 for c in confidence[:3])
    raw = [(int(v[1]*1000+.5), int(c[2])) for v, c in zip(values, confidence)]
    fast, slow = series.native_windows([raw], series.BUILD / 'real-prefix-series')
    assert fast['valid'] == slow['valid'] == 1
    assert fast['spread_mm'] == 33 and fast['rejected'] == 1
    assert abs(fast['distance_mm']/1000-truth) > 2
    assert abs(slow['distance_mm']/1000-truth) < .5


def test_firmware_rows_use_calibrated_distance_and_current_confidence():
    cases = [{'dataset': 'test', 'file': 'a', 'row': 0, 'truth_m': 2},
             {'dataset': 'test', 'file': 'a', 'row': 1, 'truth_m': 2}]
    raw = [(9.0, 9.0, 8.0, 900), (4.0, 4.0, 3.0, 900)]
    firmware = [(7200, 400, 11, 1, 1), (-2147483648, 0, 9, 1, 0)]
    rows = series.firmware_rows(cases, raw, firmware)
    assert (rows[0]['mm'], rows[0]['confidence']) == (7200, 400)
    assert (rows[1]['mm'], rows[1]['confidence']) == (-1, 0)
    assert rows[0]['truth_m'] == 2 and rows[1]['row'] == 1
    with pytest.raises(ValueError, match='counts'):
        series.firmware_rows(cases, raw[:1], firmware)
    with pytest.raises(ValueError, match='counts'):
        series.firmware_rows(cases, raw, firmware[:1])
