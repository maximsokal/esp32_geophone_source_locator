import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_forced_edges_study as study


def test_triangle_forcing_recovers_two_errors_but_cannot_certify_remaining_edges():
    cases = study.controls()
    results = study.geometry.native_solutions(cases, study.BUILD / 'test-counterexample')
    for case, result, forced in zip(cases, results, ((0, 9), (1, 6))):
        assert not result['baseline']['ok']
        assert all(result['violations'][p] == 3 for p in forced)
        decision = study.choose(case, result)
        mask = sum(1 << p for p in forced)
        assert decision['accepted'] and decision['mask'] == mask
        assert study.geometry.shape_disagreement(
            [s for s in result['solutions'] if s['mask'] == mask]) < 5
    assert study.geometry.node_error(study.choose(cases[0], results[0])['solution'], cases[0]['truth'])['max_mm'] < 5
    assert study.geometry.node_error(study.choose(cases[1], results[1])['solution'], cases[1]['truth'])['max_mm'] > 750
    # Truth is evaluation data only; it cannot alter the selector's answer.
    altered = {**cases[1], 'truth': [[0, 0, 0]] * 5, 'bad_mask': 0}
    assert study.choose(altered, results[1]) == study.choose(cases[1], results[1])
    summary = study.evaluate(cases, results)['groups']['all']
    assert summary['accepted'] == summary['recovered'] == 2
    assert summary['new_wrong_gt750'] == summary['wrong_gt750'] == 1
    assert summary['baseline_accepted'] == summary['false_exclusions'] == 0


def test_forced_selector_keeps_baseline_and_rejects_manual_missing_or_order_failure():
    case = study.controls()[0]
    result = study.geometry.native_solutions([case], study.BUILD / 'test-guards')[0]
    manual = copy.deepcopy(case)
    manual['pairs'][0]['flags'] = 64
    assert study.choose(manual, result)['reason'] == 'manual'
    missing = copy.deepcopy(case)
    missing['pairs'][0]['state'] = 4
    assert study.choose(missing, result)['reason'] == 'incomplete'
    fewer_orders = {**result, 'solutions': [s for s in result['solutions'] if s['order'] != 23]}
    assert study.choose(case, fewer_orders)['reason'] == 'not_all_orders'
    baseline = {**result, 'baseline': {**result['baseline'], 'ok': True}}
    assert study.choose(case, baseline)['solution'] is baseline['baseline']
    assert study.choose(case, baseline)['mask'] == 0
