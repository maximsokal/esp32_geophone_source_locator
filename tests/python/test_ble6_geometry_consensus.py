import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_geometry_consensus as geometry


def test_one_order_is_not_a_uniqueness_certificate():
    case = geometry.make_case('counterexample', geometry.REFERENCE, {1: 1000, 4: 1000})
    result = geometry.native_solutions([case], geometry.BUILD / 'test-order')[0]
    canonical = geometry.choose(result, all_orders=False)
    explored = geometry.choose(result, all_orders=True)
    assert not result['baseline']['ok']
    assert canonical['mask'] == 64
    assert geometry.node_error(canonical['solution'], case['truth'])['max_mm'] > 750
    assert not explored['accepted'] and explored['reason'] == 'ambiguous'


def test_clean_shape_keeps_its_distances_under_every_input_permutation():
    case = geometry.make_case('clean', geometry.REFERENCE)
    result = geometry.native_solutions([case], geometry.BUILD / 'test-clean')[0]
    full = [s for s in result['solutions'] if s['mask'] == 0]
    assert len(full) == 24
    assert max(geometry.node_error(s, case['truth'])['max_mm'] for s in full) < 5
    assert geometry.choose(result, all_orders=True)['accepted']


def test_source_manual_edge_cannot_be_automatically_removed():
    case = geometry.make_case('manual', geometry.REFERENCE, {9: 3000}, manual_mask=1 << 9)
    result = geometry.native_solutions([case], geometry.BUILD / 'test-manual')[0]
    assert all(not s['mask'] & (1 << 9) for s in result['solutions'])


def test_disagreement_uses_shape_not_absolute_rotation_or_reflection():
    a = {'nodes': [[0,0,0], [1000,0,0], [0,2000,0], [1000,2000,0]]}
    b = {'nodes': [[0,0,0], [0,1000,0], [2000,0,0], [2000,1000,0]]}
    assert geometry.shape_disagreement([a,b]) == pytest.approx(0)
    assert geometry.node_error(b, a['nodes'])['max_mm'] < 1e-6


def test_consistent_wrong_scale_is_invisible_to_residual_and_order_checks():
    case = geometry.make_case('wrong-scale', geometry.REFERENCE)
    for pair in case['pairs']:
        pair['distance'] = round(pair['distance'] * 1.5)
    result = geometry.native_solutions([case], geometry.BUILD / 'test-scale')[0]
    assert result['baseline']['ok']
    assert geometry.choose(result, all_orders=True)['accepted']
    assert geometry.node_error(result['baseline'], case['truth'])['max_mm'] > 2000
    assert max(geometry.residual_scaled_uncertainty(case,result['baseline'])) <= 750


def test_residual_scale_uses_observed_constraints_and_degrees_of_freedom():
    case = geometry.make_case('relative-sigma', geometry.REFERENCE, {0:300})
    solution = {'nodes':geometry.REFERENCE,'mask':0,'uncertainty_mm':[0,100,100,100,100]}
    scaled = geometry.residual_scaled_uncertainty(case,solution)
    # Ten observations, seven free XY coordinates; one residual is 2 sigma.
    assert scaled[1] == pytest.approx(100*(4/3)**0.5,rel=0.01)
    changed_truth = {**case,'truth':[[0,0,0]]*5}
    assert geometry.residual_scaled_uncertainty(changed_truth,solution) == scaled
    solution['mask'] = 1
    assert geometry.residual_scaled_uncertainty(case,solution) == [0,100,100,100,100]
    solution['mask'] = 7
    assert geometry.residual_scaled_uncertainty(case,solution) is None


def test_full_fit_objective_uses_all_measured_edges_and_no_true_xy():
    case = geometry.make_case('fit',geometry.REFERENCE)
    exact = {'nodes':geometry.REFERENCE,'mask':0,'order':1}
    shifted = {'nodes':[p[:] for p in geometry.REFERENCE],'mask':0,'order':0}
    shifted['nodes'][4][0] += 3000
    assert geometry.full_fit_cost(case,exact)<geometry.full_fit_cost(case,shifted)
    result = {'solutions':[shifted,exact]}
    assert geometry.choose_full_fit(case,result)['solution'] is exact
    case['truth'] = [[0,0,0]]*5
    assert geometry.choose_full_fit(case,result)['solution'] is exact
    assert not geometry.choose_full_fit(case,{'solutions':[]})['accepted']


def test_vertical_remote_does_not_block_full_mesh_in_any_input_order():
    import math
    cases = [geometry.make_case(f'vertical-{height}-{n}',
        [[0,0,0],[0,0,height],[3000,0,0],[0,4000,0],[3000,4000,0]][:n])
        for height in (-1000,0,1000) for n in (4,5)]
    results = geometry.native_solutions(cases,geometry.BUILD/'test-vertical-orders')
    for case,result in zip(cases,results):
        full = [s for s in result['solutions'] if s['mask']==0]
        assert len(full)==math.factorial(len(case['truth'])-1),case['name']
        assert max(geometry.node_error(s,case['truth'])['max_mm'] for s in full)<5
