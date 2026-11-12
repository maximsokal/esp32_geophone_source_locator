import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_methods_disagree_study as study


def test_separates_gap_reason_lost_acceptance_and_error_bases():
    cases = [{'dataset': 'A', 'truth_m': 2}, {'dataset': 'A', 'truth_m': 4},
             {'dataset': 'A', 'truth_m': None}, {'dataset': 'A', 'truth_m': 2},
             {'dataset': 'A', 'truth_m': 2}, {'dataset': 'A', 'truth_m': 2},
             {'dataset': 'B', 'truth_m': 2}]
    raw = [(3., 5., 3., 0.), (5., 5., 3., 0.), (5., 5., 3., 0.),
           (5., 5., 3., 0.), (5., 5., 3., 0.), (math.nan, 5., 3., 0.),
           (5., 5., 3., 0.)]
    legacy = [(0., 400., 600., 5.)] * len(cases)
    firmware = [(3200, 400, 11, 1, 1), (3200, 400, 11, 1, 1),
                (3200, 400, 11, 1, 1), (3200, 400, 10, 1, 1),
                (-2147483648, 0, 11, 1, 0), (3200, 400, 9, 1, 1),
                (3200, 400, 11, 2, 1)]
    result = study.summarize(cases, raw, legacy, firmware)
    a = result['A']
    assert a['total'] == 6 and a['raw_gap_gt_1m'] == 6
    assert a['methods_disagree_reason'] == 4 and a['lost_acceptance'] == 3
    assert a['lost_raw_error'] == {'n': 2, 'mae_m': 2.0, 'good_le_1m': 1, 'bad_gt_2m': 1}
    assert a['lost_calibrated_error']['n'] == 2
    assert a['lost_calibrated_error']['mae_m'] == pytest.approx(1.0)
    assert a['lost_calibrated_error']['good_le_1m'] == 1 and a['lost_calibrated_error']['bad_gt_2m'] == 0
    assert result['B']['raw_gap_gt_1m'] == 0
    assert result['B']['methods_disagree_reason'] == 1 and result['B']['lost_acceptance'] == 0
    assert result['B']['lost_raw_error']['mae_m'] is None


def test_mismatched_lengths_rejected():
    with pytest.raises(ValueError, match='counts'):
        study.summarize([{'dataset': 'A'}], [], [], [])


@pytest.mark.parametrize('legacy_confidence,current_confidence', [(400, 400), (600, 500)])
def test_lost_acceptance_requires_both_confidence_crossings(legacy_confidence, current_confidence):
    result = study.summarize([{'dataset': 'A', 'truth_m': 2}], [(3, 3, 5, 0)],
                             [(10, 400, legacy_confidence, 5)],
                             [(1200, current_confidence, 11, 1, 1)])['A']
    assert result['methods_disagree_reason'] == 1 and result['lost_acceptance'] == 0


@pytest.mark.parametrize('phase', [4.0, math.nan, -1.0])
def test_gap_requires_finite_nonnegative_phase_and_strictly_more_than_one_metre(phase):
    result = study.summarize([{'dataset': 'A'}], [(3, 3, phase, 0)],
                             [(10, 400, 600, 5)], [(1200, 400, 11, 1, 1)])['A']
    assert result['raw_gap_gt_1m'] == 0


def test_report_validation_rejects_legacy_policy_and_wrong_source(tmp_path):
    source = tmp_path/'ble6_research_replay.py'
    source.write_text('original', encoding='utf-8')
    report = {'confidence_policy': 'production with_methods', 'replayed': 2, 'owl_stride': 200,
              'sha256': {'tools\\ble6_research_replay.py': study.replay.sha256(source)}}
    study.validate_report(report, source)
    with pytest.raises(ValueError, match='with_methods'):
        study.validate_report({**report, 'confidence_policy': 'with_peak legacy'}, source)
    source.write_text('modified', encoding='utf-8')
    with pytest.raises(ValueError, match='hash mismatch'):
        study.validate_report(report, source)
