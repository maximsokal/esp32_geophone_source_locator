import sys
from pathlib import Path
import json
import subprocess
import uuid

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ble6_offset_observability as offset


XY = np.array([[0, 0], [3.2, 0.4], [0.6, 2.9], [4.1, 3.7], [1.8, 1.2]])
Z = np.array([0, 0.2, -0.1, 0.35, -0.25])


def ranges_for(xy, z, delta):
    points = np.column_stack((xy, z))
    return np.array([np.linalg.norm(points[i]-points[j]) + delta
                     for i in range(len(z)) for j in range(i+1, len(z))])


def test_grid_has_complete_deterministic_design():
    points = offset.grid_points()
    assert len(points) == 8640 and points == offset.grid_points()
    assert [p['index'] for p in points] == list(range(8640))
    grouped = {}
    for p in points:
        key = tuple(p[k] for k in ('n', 'side', 'layout', 'height_spread', 'sigma'))
        grouped.setdefault(key, set()).add(p['data_id'])
    assert len(grouped) == 360 and all(len(ids) == 1 for ids in grouped.values())
    assert {next(iter(ids)) for ids in grouped.values()} == set(range(360))
    assert len({tuple(p[k] for k in ('n', 'side', 'layout', 'height_spread', 'sigma',
                                     'true_delta', 'outlier', 'sigma0')) for p in points}) == 8640


@pytest.mark.parametrize('layout,expected', [
    ('square', [[0, 0], [10, 0], [10, 10], [0, 10], [5, 5]]),
    ('line', [[0, 0], [2.5, 0], [5, 0], [7.5, 0], [10, 0]]),
    ('L', [[0, 0], [5, 0], [10, 0], [0, 10], [0, 5]]),
    ('star', [[0, 0], [5, 0], [0, 5], [-5, 0], [0, -5]]),
])
def test_fixed_layouts_and_height_spread(layout, expected):
    point = next(p for p in offset.grid_points() if p['n'] == 5 and p['side'] == 10 and
                 p['layout'] == layout and p['height_spread'] == 1.0)
    xy, z, _ = offset.generate_observations(point, 3, 9)
    np.testing.assert_array_equal(xy, np.broadcast_to(expected, (3, 5, 2)))
    np.testing.assert_allclose(z, np.linspace(0, 1, 5))


def test_variants_share_samples_and_only_apply_requested_changes():
    point = next(p for p in offset.grid_points() if p['n'] == 5 and p['layout'] == 'random' and
                 p['height_spread'] == 0.3 and p['true_delta'] == 0 and not p['outlier'] and p['sigma0'] is None)
    xy, z, measured = offset.generate_observations(point, 30, 17)
    repeat = offset.generate_observations({**point, 'sigma0': 0.5}, 30, 17)
    for a, b in zip((xy, z, measured), repeat):
        np.testing.assert_array_equal(a, b)
    assert np.all(xy[:, 0] == 0) and np.all((xy[:, 1:] >= 0) & (xy[:, 1:] <= point['side']))
    shifted = offset.generate_observations({**point, 'true_delta': -0.9}, 30, 17)[2]
    np.testing.assert_allclose(shifted - measured, -0.9, atol=1e-14)
    outlier = offset.generate_observations({**point, 'outlier': True}, 30, 17)[2] - measured
    assert np.all(np.count_nonzero(np.isclose(outlier, 2.0), axis=1) == 1)
    assert np.all(np.isclose(outlier, 0) | np.isclose(outlier, 2))


def test_generator_retains_negative_observations():
    point = next(p for p in offset.grid_points() if p['n'] == 5 and p['side'] == 2 and
                 p['layout'] == 'line' and p['height_spread'] == 0 and p['sigma'] == 0.05 and
                 p['true_delta'] == -0.9 and not p['outlier'])
    assert np.any(offset.generate_observations(point, 3, 12)[2] < 0)


@pytest.mark.parametrize('trials,seed', [(0, 1), (-1, 1), (True, 1), (1, -1), (1, True), (1, 1.5)])
def test_generator_rejects_invalid_trials_or_seed(trials, seed):
    with pytest.raises(ValueError):
        offset.generate_observations(offset.grid_points()[0], trials, seed)


def test_batched_aligned_score_matches_scalar():
    rng = np.random.default_rng(8)
    truth = rng.normal(size=(4, 5, 2))
    predicted = rng.normal(size=(4, 5, 2))
    scores = offset._aligned_remote_rms_batch(predicted, truth)
    expected = [np.sqrt(np.mean(offset.aligned_xy_errors(a, b)[1:]**2)) for a, b in zip(predicted, truth)]
    np.testing.assert_allclose(scores, expected, atol=1e-14)


def test_evaluate_point_counts_invalid_and_never_passes_truth_to_fit(monkeypatch):
    point = offset.grid_points()[0]
    xy = np.array([XY[:4], XY[:4], XY[:4]])
    z = Z[:4]
    ranges = np.vstack((ranges_for(xy[0], z, 0), ranges_for(xy[1], z, 0), np.zeros(6)))
    monkeypatch.setattr(offset, 'generate_observations', lambda point, trials, seed: (xy, z, ranges))
    calls = []
    def fake_fit(measured, heights, **kwargs):
        calls.append((measured.copy(), heights.copy(), kwargs))
        return {'xy': xy[:2].copy(), 'delta': np.zeros(2), 'converged': np.array([True, False]),
                'observable': np.array([True, False]), 'offset_sigma': np.array([0.2, np.inf])}
    monkeypatch.setattr(offset, 'fit_ranges_batch', fake_fit)
    result = offset.evaluate_point(point, 3, 44)
    assert len(calls) == 2 and all(np.array_equal(call[0], ranges[:2]) for call in calls)
    assert calls[0][2]['seed'] == calls[1][2]['seed'] and not calls[0][2]['fit_offset'] and calls[1][2]['fit_offset']
    assert result['total'] == 3 and result['invalid'] == 1
    assert result['baseline_converged'] == result['estimated_observable'] == 1
    assert result['estimated_divergence_fraction'] == pytest.approx(2/3)
    assert result['improved_count'] == 0 and result['point']['true_delta'] == point['true_delta']
    assert result['baseline_delta'] == 0.0
    json.dumps(result, allow_nan=False)


def test_evaluate_all_invalid_keeps_full_denominator(monkeypatch):
    point = offset.grid_points()[0]
    monkeypatch.setattr(offset, 'generate_observations',
                        lambda point, trials, seed: (np.zeros((trials, 4, 2)), np.zeros(4), np.zeros((trials, 6))))
    def unexpected(*args, **kwargs):
        raise AssertionError('invalid ranges must not reach the fitter')
    monkeypatch.setattr(offset, 'fit_ranges_batch', unexpected)
    result = offset.evaluate_point(point, 4, 2)
    assert result['invalid'] == result['total'] == 4
    assert result['baseline_divergence_fraction'] == result['estimated_divergence_fraction'] == 1
    assert result['delta_bias'] is None and result['baseline_mean_node_error'] is None
    json.dumps(result, allow_nan=False)


def test_evaluate_point_is_deterministic_on_real_fits():
    point = next(p for p in offset.grid_points() if p['n'] == 5 and p['side'] == 5 and
                 p['layout'] == 'square' and p['height_spread'] == 0 and p['sigma'] == 0.05 and
                 p['true_delta'] == 0 and not p['outlier'] and p['sigma0'] is None)
    a = offset.evaluate_point(point, 3, 19, starts=2)
    b = offset.evaluate_point(point, 3, 19, starts=2)
    assert a == b and a['total'] == 3 and a['point']['true_delta'] == 0 and a['baseline_delta'] == 0
    json.dumps(a, allow_nan=False)


def test_evaluate_reports_linalg_failure(monkeypatch):
    point = offset.grid_points()[0]
    monkeypatch.setattr(offset, 'generate_observations',
                        lambda point, trials, seed: (np.zeros((trials, 4, 2)), np.zeros(4), np.ones((trials, 6))))
    def failure(*args, **kwargs):
        raise np.linalg.LinAlgError('solver failed')
    monkeypatch.setattr(offset, 'fit_ranges_batch', failure)
    result = offset.evaluate_point(point, 3, 0)
    assert result['evaluation_error'] == 'solver failed' and result['estimated_divergence_fraction'] == 1
    json.dumps(result, allow_nan=False)


def test_sweep_checkpoint_resume_matches_uninterrupted(tmp_path, monkeypatch):
    monkeypatch.setattr(offset, 'ROOT', tmp_path)
    def fake(point, trials, seed, starts):
        return {'point': dict(point), 'evaluation_error': None, 'value': point['index'] + seed + trials + starts}
    monkeypatch.setattr(offset, 'evaluate_point', fake)
    first, whole = tmp_path/'.test-build/first.json', tmp_path/'.test-build/whole.json'
    partial = offset.run_sweep(first, trials=2, seed=9, starts=1, indices=[0])
    assert len(partial['points']) == 8640 and partial['completed'] == 1 and partial['pending'] == 8639
    assert partial['header']['schema'] == 1 and partial['header']['units'] == 'm'
    assert partial['exploratory'] and not partial['complete'] and partial['decision'] is None
    resumed = offset.run_sweep(first, trials=2, seed=9, starts=1, indices=[0, 1], resume=True)
    uninterrupted = offset.run_sweep(whole, trials=2, seed=9, starts=1, indices=[0, 1])
    assert resumed == uninterrupted == json.loads(first.read_text(encoding='utf-8'))
    with pytest.raises(FileExistsError):
        offset.run_sweep(first, trials=2, seed=9, starts=1, indices=[0])
    with pytest.raises(ValueError):
        offset.run_sweep(first, trials=3, seed=9, starts=1, indices=[0], resume=True)
    fake_source = tmp_path/'modified.py'
    fake_source.write_text('different source', encoding='utf-8')
    monkeypatch.setattr(offset, '__file__', str(fake_source))
    with pytest.raises(ValueError):
        offset.run_sweep(first, trials=2, seed=9, starts=1, indices=[0], resume=True)


def test_sweep_rejects_corrupt_duplicate_and_invalid_arguments(tmp_path, monkeypatch):
    monkeypatch.setattr(offset, 'ROOT', tmp_path)
    monkeypatch.setattr(offset, 'evaluate_point',
                        lambda point, trials, seed, starts: {'point': dict(point), 'evaluation_error': None})
    output = tmp_path/'.test-build/report.json'
    offset.run_sweep(output, trials=1, indices=[0])
    checkpoint = output.with_suffix('.jsonl')
    original = checkpoint.read_text(encoding='utf-8')
    checkpoint.write_text(original + original.splitlines()[-1] + '\n', encoding='utf-8')
    with pytest.raises(ValueError):
        offset.run_sweep(output, trials=1, indices=[0], resume=True)
    checkpoint.write_text(original + '{bad\n', encoding='utf-8')
    with pytest.raises(ValueError):
        offset.run_sweep(output, trials=1, indices=[0], resume=True)
    checkpoint.write_text(original.replace('"evaluation_error": null', '"evaluation_error": null, "bad": 1e999'), encoding='utf-8')
    with pytest.raises(ValueError):
        offset.run_sweep(output, trials=1, indices=[0], resume=True)
    checkpoint.write_text(original.replace('"evaluation_error": null', '"evaluation_error": null, "total": 2'), encoding='utf-8')
    with pytest.raises(ValueError):
        offset.run_sweep(output, trials=1, indices=[0], resume=True)
    for kwargs in ({'indices': [-1]}, {'indices': [8640]}, {'indices': [1, 1]},
                   {'trials': 0}, {'seed': -1}, {'starts': 0}):
        with pytest.raises(ValueError):
            offset.run_sweep(tmp_path/'.test-build/new.json', **kwargs)
    with pytest.raises(ValueError):
        offset.run_sweep(tmp_path/'outside.json', trials=1, indices=[0])


def test_sweep_records_nonfinite_metrics_as_error(tmp_path, monkeypatch):
    monkeypatch.setattr(offset, 'ROOT', tmp_path)
    monkeypatch.setattr(offset, 'evaluate_point',
                        lambda point, trials, seed, starts: {'point': dict(point), 'value': float('nan')})
    output = tmp_path/'.test-build/report.json'
    with pytest.raises(ValueError):
        offset.run_sweep(output, trials=1, indices=[0])
    report = json.loads(output.read_text(encoding='utf-8'))
    assert report['errors'] == 1 and report['points'][0]['status'] == 'error'
    json.dumps(report, allow_nan=False)


def test_sweep_stops_after_unexpected_error_and_saves_it(tmp_path, monkeypatch):
    monkeypatch.setattr(offset, 'ROOT', tmp_path)
    seen = []
    def failure(point, trials, seed, starts):
        seen.append(point['index'])
        raise RuntimeError('unexpected failure')
    monkeypatch.setattr(offset, 'evaluate_point', failure)
    output = tmp_path/'.test-build/report.json'
    with pytest.raises(RuntimeError, match='unexpected failure'):
        offset.run_sweep(output, trials=1, indices=[0, 1])
    assert seen == [0]
    report = json.loads(output.read_text(encoding='utf-8'))
    checkpoint = output.with_suffix('.jsonl').read_text(encoding='utf-8').splitlines()
    assert len(checkpoint) == 2 and json.loads(checkpoint[1])['status'] == 'error'
    assert report['errors'] == 1 and report['pending'] == 8639
    assert report['points'][0]['status'] == 'error' and report['points'][1]['status'] == 'pending'


def test_sweep_cli_tiny_real_smoke():
    output = offset.ROOT/'.test-build'/'offset-cli-smoke'/f'{uuid.uuid4().hex}.json'
    process = subprocess.run([sys.executable, str(Path(offset.__file__).resolve()), '--output', str(output),
                              '--trials', '1', '--starts', '1', '--point', '0'],
                             cwd=offset.ROOT, capture_output=True, text=True)
    assert process.returncode == 0, process.stderr
    report = json.loads(output.read_text(encoding='utf-8'))
    assert report['completed'] == 1 and report['pending'] == 8639 and report['exploratory']


@pytest.mark.parametrize('n', [4, 5])
@pytest.mark.parametrize('fit_offset', [False, True])
@pytest.mark.parametrize('sigma0', [None, 0.5])
def test_batched_model_matches_scalar_and_finite_difference(n, fit_offset, sigma0):
    rng = np.random.default_rng(20261112)
    edges = [(i, j) for i in range(n) for j in range(i+1, n)]
    params = rng.normal(size=(7, 2*n-3 + int(fit_offset)))
    z = Z[:n]
    prediction, jacobian, xy = offset._model_batch(params, z, edges, fit_offset, 0.15, sigma0)
    for i, values in enumerate(params):
        scalar, scalar_jacobian = offset._model(values, z, edges, fit_offset, 0.15, sigma0)
        np.testing.assert_allclose(xy[i], offset._coordinates(values, n), rtol=0, atol=0)
        np.testing.assert_allclose(prediction[i], scalar, rtol=1e-14, atol=1e-14)
        np.testing.assert_allclose(jacobian[i], scalar_jacobian, rtol=1e-14, atol=1e-14)
    step = 1e-6
    for column in range(params.shape[1]):
        perturbation = np.zeros_like(params)
        perturbation[:, column] = step
        plus = offset._model_batch(params + perturbation, z, edges, fit_offset, 0.15, sigma0)[0]
        minus = offset._model_batch(params - perturbation, z, edges, fit_offset, 0.15, sigma0)[0]
        np.testing.assert_allclose(jacobian[:, :, column], (plus-minus)/(2*step), rtol=1e-6, atol=1e-8)


def test_batched_model_handles_coincident_nodes_like_scalar():
    params = np.zeros((2, 6))
    z = np.zeros(4)
    edges = [(i, j) for i in range(4) for j in range(i+1, 4)]
    prediction, jacobian, _ = offset._model_batch(params, z, edges, True, 1.0, 0.5)
    scalar, scalar_jacobian = offset._model(params[0], z, edges, True, 1.0, 0.5)
    np.testing.assert_array_equal(prediction[0], scalar)
    np.testing.assert_array_equal(jacobian[0], scalar_jacobian)


@pytest.mark.parametrize('fit_offset,sigma0', [(True, None), (True, 0.5), (False, None)])
def test_batch_fit_matches_scalar_on_clean_and_noisy_ranges(fit_offset, sigma0):
    rng = np.random.default_rng(18)
    clean = ranges_for(XY, Z, 0.3)
    ranges = np.vstack((clean, clean + rng.normal(0, 0.01, len(clean))))
    batch = offset.fit_ranges_batch(ranges, Z, sigma=0.15, sigma0=sigma0, fit_offset=fit_offset, seed=17)
    for i in range(len(ranges)):
        scalar = offset.fit_ranges(ranges[i], Z, sigma=0.15, sigma0=sigma0, fit_offset=fit_offset, seed=17)
        assert batch['converged'][i] and scalar['converged']
        assert batch['cost'][i] == pytest.approx(scalar['cost'], abs=1e-6)
        assert batch['delta'][i] == pytest.approx(scalar['delta'], abs=1e-3)
        assert max(offset.aligned_xy_errors(batch['xy'][i], scalar['xy'])) < 1e-3
        assert batch['observable'][i] == scalar['observable']
        if fit_offset and batch['observable'][i]:
            assert batch['offset_sigma'][i] == pytest.approx(np.sqrt(scalar['covariance'][-1, -1]), rel=1e-3)
        else:
            assert np.isinf(batch['offset_sigma'][i])


def test_batch_fit_is_deterministic_and_rejects_degenerate_geometry():
    z = np.zeros(5)
    line = np.column_stack((np.arange(5)*2.0, z))
    ranges = np.vstack((ranges_for(line, z, 0), ranges_for(XY, z, 0.3)))
    a = offset.fit_ranges_batch(ranges, z, seed=6)
    b = offset.fit_ranges_batch(ranges, z, seed=6)
    for key in a:
        np.testing.assert_array_equal(a[key], b[key])
    assert not a['observable'][0] and np.isinf(a['offset_sigma'][0])


@pytest.mark.parametrize('ranges,z,kwargs', [
    (np.ones((0, 10)), Z, {}), (np.ones(10), Z, {}), (np.ones((2, 9)), Z, {}),
    (np.full((2, 10), np.nan), Z, {}), (np.zeros((1, 10)), Z, {}),
    (np.ones((1, 10)), [0, 0, 0, 0, np.inf], {}),
    (np.ones((1, 10)), Z, {'sigma': 0}), (np.ones((1, 10)), Z, {'sigma0': np.nan}),
    (np.ones((1, 10)), Z, {'starts': 0}),
])
def test_batch_fit_rejects_invalid_inputs(ranges, z, kwargs):
    with pytest.raises(ValueError):
        offset.fit_ranges_batch(ranges, z, **kwargs)


@pytest.mark.parametrize('delta', [-0.9, 0.0, 0.9])
def test_noiseless_offset_and_xy_recovery(delta):
    result = offset.fit_ranges(ranges_for(XY, Z, delta), Z, sigma=0.15, seed=17)
    assert result['converged'] and result['observable']
    assert abs(result['delta'] - delta) < 0.001
    assert max(offset.aligned_xy_errors(result['xy'], XY)) < 0.001
    baseline = offset.fit_ranges(ranges_for(XY, Z, delta), Z, fit_offset=False, seed=17)
    assert baseline['converged'] and baseline['delta'] == 0
    assert baseline['stationarity'] < 1e-7
    if delta:
        assert max(offset.aligned_xy_errors(baseline['xy'], XY)) > 0.001


def test_fixed_seed_repeats_fit_and_prior_affects_solution():
    measured = ranges_for(XY, Z, 0.9)
    a = offset.fit_ranges(measured, Z, sigma=0.15, sigma0=0.3, seed=6)
    b = offset.fit_ranges(measured, Z, sigma=0.15, sigma0=0.3, seed=6)
    assert np.array_equal(a['xy'], b['xy']) and a['delta'] == b['delta']
    assert abs(a['delta']) < 0.9
    assert a['covariance'] is not None


@pytest.mark.parametrize('ranges,z,kwargs', [
    ([1] * 9, Z, {}), ([1] * 9 + [float('nan')], Z, {}),
    ([1] * 9 + [0], Z, {}), ([1] * 10, [0, 0, 0, 0, float('inf')], {}),
    ([1] * 10, Z, {'sigma': 0}), ([1] * 10, Z, {'sigma0': float('nan')}),
])
def test_invalid_input(ranges, z, kwargs):
    with pytest.raises(ValueError):
        offset.fit_ranges(ranges, z, **kwargs)


def test_collinear_geometry_is_not_confidently_observable():
    xy = np.column_stack((np.arange(5) * 2.0, np.zeros(5)))
    result = offset.fit_ranges(ranges_for(xy, np.zeros(5), 0), np.zeros(5), seed=8)
    assert not result['observable']
    assert result['covariance'] is None
