"""Behavior of the research summary over raw controls and firmware outcomes."""
import importlib.util
import math
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ble6_summary_replay', ROOT/'tools/ble6_research_replay.py')
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


def test_summary_uses_calibrated_distance_and_current_confidence():
    base = {'dataset': 'OCMTEC', 'file': 'measured.csv', 'row': 0,
            'truth_m': 1.0, 'author_ifft': 5.0, 'rtt': math.nan}
    cases = [base, {**base, 'row': 1}, {**base, 'row': 2},
             {**base, 'file': 'all-outliers.csv', 'row': 3},
             {'dataset': 'Waves', 'file': 'waves', 'row': 4, 'truth_m': None}]
    raw = [(5.0, 5.0, 4.9, 900.0)] * len(cases)
    firmware = [(2000, 400, 11, 1, 1), (3000, 600, 1, 1, 1),
                (-2147483648, 0, 9, 1, 0), (-2147483648, 0, 9, 1, 0),
                (3200, 700, 1, 1, 1)]
    groups, orientation, examples, waves, reference = replay.summarize_replay(cases, raw, firmware, 0.5)
    group = groups['OCMTEC:measured.csv']
    assert group['total'] == 3 and group['n'] == 2 and group['unavailable_or_outlier'] == 1
    assert group['mae_m'] == 1.5 and group['large_error'] == 2
    assert group['high_confidence_large_error'] == 1
    assert len(examples['OCMTEC']) == 1 and examples['OCMTEC'][0]['distance_m'] == 3.0
    assert examples['OCMTEC'][0]['confidence'] == 600
    outliers = groups['OCMTEC:all-outliers.csv']
    assert outliers['total'] == 1 and outliers['n'] == 0 and outliers['unavailable_or_outlier'] == 1
    assert outliers['mae_m'] is None and outliers['median_error_m'] is None and outliers['p95_error_m'] is None
    assert reference == [0.0, 0.0, 0.0, 0.0] and orientation == {}
    assert waves == [{'procedure': 4, 'raw_m': 5.0, 'filled_m': 5.0, 'phase_m': 4.9,
                      'calibrated_m': 3.2, 'valid': True, 'confidence': 700}]


def test_summary_rejects_mismatched_record_counts():
    with pytest.raises(ValueError, match='record counts'):
        replay.summarize_replay([{}], [], [], 1.0)
