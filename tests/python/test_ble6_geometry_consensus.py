import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_geometry_consensus as geometry


@pytest.mark.parametrize('heights',[[0,0,0,0,0],[0,200,-300,100,-50]])
def test_three_collinear_references_cannot_resolve_an_independent_mirror(heights):
    points=[[0,0,0],[4000,0,0],[8000,0,0],[2000,3000,0],[6000,-3000,0]]
    for p,z in zip(points,heights):p[2]=z
    case=geometry.make_case('independent-mirror',points,missing_mask=1<<9)
    mirrored=[p[:] for p in points];mirrored[4][1]*=-1
    other=geometry.make_case('same-observed-edges',mirrored,missing_mask=1<<9)
    assert all(a['distance']==b['distance'] for a,b in zip(case['pairs'][:9],other['pairs'][:9]))
    assert abs(case['pairs'][9]['distance']-other['pairs'][9]['distance'])>750
    result=geometry.native_solutions([case],geometry.BUILD/'test-independent-mirror')[0]
    assert not result['baseline']['ok']
    assert not any(s['mask']==0 for s in result['solutions'])


@pytest.mark.parametrize('heights',[[0,0,0,0,0],[0,200,-300,100,-50]])
def test_measured_mirror_link_restores_identifiability_without_removing_edges(heights):
    points=[[0,0,0],[4000,0,0],[8000,0,0],[2000,3000,0],[6000,-3000,0]]
    for p,z in zip(points,heights):p[2]=z
    case=geometry.make_case('mirror-link',points,manual_mask=1<<9)
    result=geometry.native_solutions([case],geometry.BUILD/'test-mirror-link')[0]
    assert result['baseline']['ok']
    assert geometry.node_error(result['baseline'],points)['max_mm']<10


def test_mirror_study_has_identical_observed_edges_for_incompatible_truths():
    import ble6_mirror_study as study
    rows=study.cases(110420261,2)
    assert len(rows)==8 and rows==study.cases(110420261,2)
    for at in (0,4):
        a,b=rows[at:at+2]
        assert all(a['pairs'][p]['distance']==b['pairs'][p]['distance'] for p in range(9))
        assert abs(a['pairs'][9]['distance']-b['pairs'][9]['distance'])>750
        assert rows[at+2]['pairs'][9]['state']==2


def test_noncollinear_partial_controls_are_not_classified_by_coarse_residual_tolerance():
    names={f'8112026/{i}/nodes5/missing' for i in (9,10,12)}
    cases=[c for c in geometry.synthetic_cases(8112026,16) if c['name'] in names]
    assert len(cases)==3
    results=geometry.native_solutions(cases,geometry.BUILD/'test-noncollinear-controls')
    for c,r in zip(cases,results):
        assert r['baseline']['ok']
        assert geometry.node_error(r['baseline'],c['truth'])['max_mm']<5


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
