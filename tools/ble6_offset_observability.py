"""Fit a shared range offset and planar coordinates from complete pair ranges.

Distances, heights and sigma use the same units. Pair ranges are ordered (0,1),
(0,2), ..., (n-2,n-1). This diagnostic is not a production solver.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
from itertools import product

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def _coordinates(params, n):
    xy = np.zeros((n, 2))
    xy[1, 0] = params[0]
    xy[2:] = params[1:2*n-3].reshape(n-2, 2)
    return xy


def _initial_xy(ranges, z, edges, offset):
    n = len(z)
    squared = np.zeros((n, n))
    for value, (i, j) in zip(ranges, edges):
        squared[i, j] = squared[j, i] = max((value-offset)**2-(z[i]-z[j])**2, 0)
    center = np.eye(n) - np.ones((n, n))/n
    values, vectors = np.linalg.eigh(-0.5 * center @ squared @ center)
    xy = vectors[:, -2:] * np.sqrt(np.maximum(values[-2:], 0))
    xy -= xy[0]
    angle = np.arctan2(xy[1, 1], xy[1, 0])
    rotation = np.array([[np.cos(angle), -np.sin(angle)], [np.sin(angle), np.cos(angle)]])
    return xy @ rotation


def _model(params, z, edges, fit_offset, sigma, sigma0):
    n = len(z)
    xy = _coordinates(params, n)
    delta = params[-1] if fit_offset else 0.0
    rows, predictions = [], []
    for i, j in edges:
        diff = xy[i] - xy[j]
        distance = np.hypot(np.linalg.norm(diff), z[i]-z[j])
        direction = diff / distance if distance > 1e-15 else np.zeros(2)
        row = np.zeros(len(params))
        for node, sign in ((i, 1), (j, -1)):
            if node == 1:
                row[0] += sign * direction[0]
            elif node >= 2:
                row[1+2*(node-2):3+2*(node-2)] += sign * direction
        if fit_offset:
            row[-1] = 1
        rows.append(row / sigma)
        predictions.append((distance + delta) / sigma)
    if fit_offset and sigma0 is not None:
        row = np.zeros(len(params))
        row[-1] = 1 / sigma0
        rows.append(row)
        predictions.append(delta / sigma0)
    return np.asarray(predictions), np.asarray(rows)


def _coordinates_batch(params, n):
    """Convert a (batch, parameters) array into (batch, nodes, 2) coordinates."""
    xy = np.zeros((len(params), n, 2))
    xy[:, 1, 0] = params[:, 0]
    xy[:, 2:] = params[:, 1:2*n-3].reshape(len(params), n-2, 2)
    return xy


def _model_batch(params, z, edges, fit_offset, sigma, sigma0):
    """Return predictions, Jacobians and XY for a batch with shared heights."""
    xy = _coordinates_batch(params, len(z))
    prior = fit_offset and sigma0 is not None
    predictions = np.zeros((len(params), len(edges) + int(prior)))
    jacobian = np.zeros((len(params), predictions.shape[1], params.shape[1]))
    delta = params[:, -1] if fit_offset else 0.0
    for edge, (i, j) in enumerate(edges):
        diff = xy[:, i] - xy[:, j]
        distance = np.hypot(np.linalg.norm(diff, axis=1), z[i] - z[j])
        direction = np.divide(diff, distance[:, None], out=np.zeros_like(diff),
                              where=distance[:, None] > 1e-15)
        predictions[:, edge] = (distance + delta) / sigma
        for node, sign in ((i, 1), (j, -1)):
            if node == 1:
                jacobian[:, edge, 0] += sign * direction[:, 0] / sigma
            elif node >= 2:
                jacobian[:, edge, 1+2*(node-2):3+2*(node-2)] += sign * direction / sigma
        if fit_offset:
            jacobian[:, edge, -1] = 1 / sigma
    if prior:
        predictions[:, -1] = delta / sigma0
        jacobian[:, -1, -1] = 1 / sigma0
    return predictions, jacobian, xy


def fit_ranges(ranges, z, *, sigma=1.0, sigma0=None, fit_offset=True, seed=0, starts=12):
    """Return best multistart weighted least-squares fit and local diagnostics.

    sigma0 is a zero-mean Gaussian offset prior; None disables it. Covariance
    uses the supplied measurement sigma and prior, without residual scaling.
    Data observability excludes the prior, which cannot create geometric evidence.
    Stationarity is the infinity norm of J.T @ residual divided by
    max(1, ||J||_2 * ||residual||_2); convergence requires it below 1e-7.
    A nearly collinear fit can stop just off the line with a numerically full
    rank Jacobian, so shapes with minor/major XY singular value <= 1e-3 are
    conservatively reported as unobservable as well.
    """
    ranges = np.asarray(ranges, dtype=float)
    z = np.asarray(z, dtype=float)
    if z.ndim != 1 or len(z) < 4 or not np.all(np.isfinite(z)):
        raise ValueError('z must contain at least four finite heights')
    n = len(z)
    edges = [(i, j) for i in range(n) for j in range(i+1, n)]
    if ranges.shape != (len(edges),) or not np.all(np.isfinite(ranges)) or np.any(ranges <= 0):
        raise ValueError('ranges must contain one positive finite value per pair')
    if not np.isfinite(sigma) or sigma <= 0 or (sigma0 is not None and (not np.isfinite(sigma0) or sigma0 <= 0)):
        raise ValueError('sigma and sigma0 must be positive and finite')
    if not isinstance(starts, int) or isinstance(starts, bool) or starts < 1:
        raise ValueError('starts must be a positive integer')
    rng = np.random.default_rng(seed)
    target = ranges / sigma
    if fit_offset and sigma0 is not None:
        target = np.append(target, 0.0)
    best = None
    for start in range(starts):
        offset = (0.0 if start == 0 or not fit_offset else rng.uniform(-0.4, 0.4) * np.median(ranges))
        xy = _initial_xy(ranges, z, edges, offset)
        if start:
            xy[2:] += rng.normal(0, 0.15 * np.median(ranges), (n-2, 2))
        params = np.r_[xy[1, 0], xy[2:].ravel(), [offset] if fit_offset else []]
        damping, converged = 1e-3, False
        for iteration in range(250):
            prediction, jacobian = _model(params, z, edges, fit_offset, sigma, sigma0)
            residual = prediction - target
            cost = residual @ residual
            gradient = jacobian.T @ residual
            stationarity = np.linalg.norm(gradient, ord=np.inf) / max(
                1.0, np.linalg.norm(jacobian, ord=2) * np.linalg.norm(residual))
            if stationarity < 1e-7:
                converged = True
                break
            step = np.linalg.solve(jacobian.T @ jacobian + damping * np.eye(len(params)), -gradient)
            candidate = params + step
            trial = _model(candidate, z, edges, fit_offset, sigma, sigma0)[0] - target
            if trial @ trial < cost:
                params = candidate
                damping = max(damping / 3, 1e-12)
            else:
                damping = min(damping * 10, 1e15)
        prediction, jacobian = _model(params, z, edges, fit_offset, sigma, sigma0)
        residual = prediction - target
        cost = float(residual @ residual)
        stationarity = float(np.linalg.norm(jacobian.T @ residual, ord=np.inf) / max(
            1.0, np.linalg.norm(jacobian, ord=2) * np.linalg.norm(residual)))
        converged = stationarity < 1e-7
        if best is None or cost < best[0]:
            best = cost, params, jacobian, converged, iteration+1, stationarity
    cost, params, jacobian, converged, iterations, stationarity = best
    data_jacobian = jacobian[:len(edges)]
    singular = np.linalg.svd(data_jacobian, compute_uv=False)
    condition = float(singular[0] / singular[-1]) if singular[-1] > 0 else float('inf')
    shape = np.linalg.svd(_coordinates(params, n), compute_uv=False)
    observable = bool(converged and len(singular) >= len(params) and condition < 1e6
                      and shape[-1] > 1e-3 * shape[0])
    covariance = np.linalg.pinv(jacobian.T @ jacobian) if observable else None
    return {'xy': _coordinates(params, n), 'delta': float(params[-1]) if fit_offset else 0.0,
            'cost': cost, 'converged': converged, 'iterations': iterations,
            'stationarity': stationarity, 'condition': condition,
            'observable': observable, 'covariance': covariance}


def fit_ranges_batch(ranges, z, *, sigma=1.0, sigma0=None, fit_offset=True, seed=0, starts=12):
    """Fit independent range rows with shared heights; offset_sigma uses the supplied sigma and prior."""
    ranges, z = np.asarray(ranges, dtype=float), np.asarray(z, dtype=float)
    if z.ndim != 1 or len(z) < 4 or not np.all(np.isfinite(z)):
        raise ValueError('z must contain at least four finite heights')
    n = len(z)
    edges = [(i, j) for i in range(n) for j in range(i+1, n)]
    if ranges.ndim != 2 or ranges.shape[0] == 0 or ranges.shape[1] != len(edges) or not np.all(np.isfinite(ranges)) or np.any(ranges <= 0):
        raise ValueError('ranges must contain positive finite pair values in each row')
    if not np.isfinite(sigma) or sigma <= 0 or (sigma0 is not None and (not np.isfinite(sigma0) or sigma0 <= 0)):
        raise ValueError('sigma and sigma0 must be positive and finite')
    if not isinstance(starts, int) or isinstance(starts, bool) or starts < 1:
        raise ValueError('starts must be a positive integer')
    batch, p = len(ranges), 2*n-3 + int(fit_offset)
    target = ranges / sigma
    if fit_offset and sigma0 is not None:
        target = np.column_stack((target, np.zeros(batch)))
    rng = np.random.default_rng(seed)
    scale = np.median(ranges, axis=1)
    best_cost = np.full(batch, np.inf)
    best_params = np.zeros((batch, p))
    center = np.eye(n) - np.ones((n, n))/n
    identity = np.eye(p)
    for start in range(starts):
        initial_delta = np.zeros(batch) if start == 0 or not fit_offset else rng.uniform(-0.4, 0.4, batch) * scale
        squared = np.zeros((batch, n, n))
        for edge, (i, j) in enumerate(edges):
            squared[:, i, j] = squared[:, j, i] = np.maximum(
                (ranges[:, edge] - initial_delta)**2 - (z[i]-z[j])**2, 0)
        values, vectors = np.linalg.eigh(-0.5 * (center @ squared @ center))
        xy = vectors[:, :, -2:] * np.sqrt(np.maximum(values[:, None, -2:], 0))
        xy -= xy[:, :1]
        angle = np.arctan2(xy[:, 1, 1], xy[:, 1, 0])
        cosine, sine = np.cos(angle)[:, None], np.sin(angle)[:, None]
        x, y = xy[:, :, 0].copy(), xy[:, :, 1].copy()
        xy[:, :, 0], xy[:, :, 1] = x*cosine + y*sine, -x*sine + y*cosine
        if start:
            xy[:, 2:] += rng.normal(size=(batch, n-2, 2)) * (0.15*scale[:, None, None])
        params = np.column_stack((xy[:, 1, 0], xy[:, 2:].reshape(batch, -1)))
        if fit_offset:
            params = np.column_stack((params, initial_delta))
        damping = np.full(batch, 1e-3)
        active = np.ones(batch, dtype=bool)
        for _ in range(250):
            indices = np.flatnonzero(active)
            if not len(indices):
                break
            prediction, jacobian, _ = _model_batch(params[indices], z, edges, fit_offset, sigma, sigma0)
            residual = prediction - target[indices]
            gradient = np.einsum('bep,be->bp', jacobian, residual)
            stationarity = np.max(np.abs(gradient), axis=1) / np.maximum(
                1.0, np.linalg.norm(jacobian, ord=2, axis=(1, 2)) * np.linalg.norm(residual, axis=1))
            active[indices[stationarity < 1e-7]] = False
            remaining = stationarity >= 1e-7
            indices, jacobian, gradient, residual = indices[remaining], jacobian[remaining], gradient[remaining], residual[remaining]
            if not len(indices):
                break
            normal = np.swapaxes(jacobian, 1, 2) @ jacobian + damping[indices, None, None] * identity
            step = np.linalg.solve(normal, -gradient[..., None])[..., 0]
            candidate = params[indices] + step
            trial = _model_batch(candidate, z, edges, fit_offset, sigma, sigma0)[0] - target[indices]
            improved = np.sum(trial*trial, axis=1) < np.sum(residual*residual, axis=1)
            params[indices[improved]] = candidate[improved]
            damping[indices] = np.where(improved, np.maximum(damping[indices]/3, 1e-12),
                                        np.minimum(damping[indices]*10, 1e15))
        residual = _model_batch(params, z, edges, fit_offset, sigma, sigma0)[0] - target
        cost = np.sum(residual*residual, axis=1)
        better = cost < best_cost
        best_cost[better], best_params[better] = cost[better], params[better]
    prediction, jacobian, xy = _model_batch(best_params, z, edges, fit_offset, sigma, sigma0)
    residual = prediction - target
    gradient = np.einsum('bep,be->bp', jacobian, residual)
    stationarity = np.max(np.abs(gradient), axis=1) / np.maximum(
        1.0, np.linalg.norm(jacobian, ord=2, axis=(1, 2)) * np.linalg.norm(residual, axis=1))
    converged = stationarity < 1e-7
    singular = np.linalg.svd(jacobian[:, :len(edges)], compute_uv=False)
    condition = np.divide(singular[:, 0], singular[:, -1], out=np.full(batch, np.inf), where=singular[:, -1] > 0)
    shape = np.linalg.svd(xy, compute_uv=False)
    observable = converged & (singular.shape[1] >= p) & (condition < 1e6) & (shape[:, -1] > 1e-3*shape[:, 0])
    offset_sigma = np.full(batch, np.inf)
    if fit_offset and np.any(observable):
        j = jacobian[observable]
        covariance = np.linalg.pinv(np.swapaxes(j, 1, 2) @ j)
        offset_sigma[observable] = np.sqrt(np.maximum(covariance[:, -1, -1], 0))
    return {'xy': xy, 'delta': best_params[:, -1] if fit_offset else np.zeros(batch),
            'cost': best_cost, 'converged': converged, 'stationarity': stationarity,
            'condition': condition, 'observable': observable, 'offset_sigma': offset_sigma}


def grid_points():
    """Enumerate simulation settings, sharing data_id across offset, outlier and prior variants."""
    points = []
    data_grid = product((4, 5), (2, 5, 10, 20), ('random', 'square', 'line', 'L', 'star'),
                        (0, 0.3, 1.0), (0.05, 0.15, 0.3))
    for data_id, (n, side, layout, height_spread, sigma) in enumerate(data_grid):
        for true_delta, outlier, sigma0 in product((-0.9, 0, 0.9), (False, True), (None, 0.3, 0.5, 1.0)):
            points.append({'index': len(points), 'data_id': data_id, 'n': n, 'side': side,
                           'layout': layout, 'height_spread': height_spread, 'sigma': sigma,
                           'true_delta': true_delta, 'outlier': outlier, 'sigma0': sigma0})
    return points


def generate_observations(point, trials, seed):
    """Generate matched layouts and raw measurements for a simulation grid point."""
    if not isinstance(trials, int) or isinstance(trials, bool) or trials < 1:
        raise ValueError('trials must be a positive integer')
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        raise ValueError('seed must be a non-negative integer')
    n, side = point['n'], point['side']
    rng = np.random.default_rng(np.random.SeedSequence([seed, point['data_id']]))
    if point['layout'] == 'random':
        xy = np.zeros((trials, n, 2))
        xy[:, 1:] = rng.uniform(0, side, (trials, n-1, 2))
    else:
        layouts = {
            'square': [(0, 0), (side, 0), (side, side), (0, side), (side/2, side/2)],
            'line': [(x, 0) for x in np.linspace(0, side, n)],
            'L': [(0, 0), (side/2, 0), (side, 0), (0, side), (0, side/2)],
            'star': [(0, 0), (side/2, 0), (0, side/2), (-side/2, 0), (0, -side/2)],
        }
        xy = np.broadcast_to(np.asarray(layouts[point['layout']][:n], dtype=float), (trials, n, 2)).copy()
    z = np.linspace(0, point['height_spread'], n)
    edges = [(i, j) for i in range(n) for j in range(i+1, n)]
    distances = np.stack([np.hypot(np.linalg.norm(xy[:, i]-xy[:, j], axis=1), z[i]-z[j])
                          for i, j in edges], axis=1)
    measured = distances + rng.normal(0, point['sigma'], distances.shape) + point['true_delta']
    if point['outlier']:
        edge = rng.integers(0, len(edges), size=trials)
        measured[np.arange(trials), edge] += 2.0
    return xy, z, measured


def _aligned_remote_rms_batch(xy, truth_xy):
    """Remove translation and orthogonal XY gauge before scoring remote nodes."""
    xy, truth = xy - xy[:, :1], truth_xy - truth_xy[:, :1]
    u, _, vt = np.linalg.svd(np.swapaxes(xy, 1, 2) @ truth)
    difference = xy @ (u @ vt) - truth
    return np.sqrt(np.mean(np.sum(difference[:, 1:]**2, axis=2), axis=1))


def evaluate_point(point, trials, seed, starts=12):
    """Evaluate one grid point; failed observations remain in every fraction's denominator."""
    truth, z, measured = generate_observations(point, trials, seed)
    valid = np.all(np.isfinite(measured) & (measured > 0), axis=1)
    count = int(np.count_nonzero(valid))
    result = {'point': dict(point), 'baseline_delta': 0.0,
              'total': trials, 'invalid': trials-count, 'baseline_converged': 0,
              'baseline_observable': 0, 'estimated_converged': 0, 'estimated_observable': 0,
              'baseline_divergence_fraction': 1.0, 'estimated_divergence_fraction': 1.0,
              'improved_count': 0, 'improved_fraction': 0.0,
              'delta_bias': None, 'delta_std': None, 'delta_rmse': None,
              'baseline_mean_node_error': None, 'estimated_mean_node_error': None,
              'offset_sigma_median': None, 'offset_sigma_p95': None, 'evaluation_error': None}
    if not count:
        return result
    fit_seed = int(np.random.SeedSequence([seed, point['data_id'], 1]).generate_state(1)[0])
    try:
        baseline = fit_ranges_batch(measured[valid], z, sigma=point['sigma'], fit_offset=False,
                                    seed=fit_seed, starts=starts)
        estimated = fit_ranges_batch(measured[valid], z, sigma=point['sigma'], sigma0=point['sigma0'],
                                     fit_offset=True, seed=fit_seed, starts=starts)
        base_ok = baseline['converged'] & baseline['observable']
        est_ok = estimated['converged'] & estimated['observable']
        base_error = _aligned_remote_rms_batch(baseline['xy'], truth[valid])
        est_error = _aligned_remote_rms_batch(estimated['xy'], truth[valid])
    except np.linalg.LinAlgError as error:
        result['evaluation_error'] = str(error) or 'linear algebra failure'
        return result
    result['baseline_converged'] = int(np.count_nonzero(baseline['converged']))
    result['estimated_converged'] = int(np.count_nonzero(estimated['converged']))
    result['baseline_observable'] = int(np.count_nonzero(base_ok))
    result['estimated_observable'] = int(np.count_nonzero(est_ok))
    result['baseline_divergence_fraction'] = 1.0 - result['baseline_converged']/trials
    result['estimated_divergence_fraction'] = 1.0 - result['estimated_converged']/trials
    paired = base_ok & est_ok
    result['improved_count'] = int(np.count_nonzero(paired & (est_error < base_error)))
    result['improved_fraction'] = result['improved_count']/trials
    if np.any(base_ok):
        result['baseline_mean_node_error'] = float(np.mean(base_error[base_ok]))
    if np.any(est_ok):
        delta_error = estimated['delta'][est_ok] - point['true_delta']
        result['delta_bias'] = float(np.mean(delta_error))
        result['delta_std'] = float(np.std(delta_error))
        result['delta_rmse'] = float(np.sqrt(np.mean(delta_error**2)))
        result['estimated_mean_node_error'] = float(np.mean(est_error[est_ok]))
        result['offset_sigma_median'] = float(np.median(estimated['offset_sigma'][est_ok]))
        result['offset_sigma_p95'] = float(np.percentile(estimated['offset_sigma'][est_ok], 95))
    return result


def run_sweep(output, trials=2000, seed=20261112, starts=12, indices=None, resume=False):
    """Checkpoint selected points and write a complete-grid report under .test-build."""
    for value, name, minimum in ((trials, 'trials', 1), (seed, 'seed', 0), (starts, 'starts', 1)):
        if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
            raise ValueError(f'{name} must be an integer >= {minimum}')
    output = Path(output).resolve()
    build = (ROOT / '.test-build').resolve()
    if not output.is_relative_to(build) or output == build or output.suffix != '.json':
        raise ValueError('output must be a .json file under repository .test-build')
    points = grid_points()
    selected = list(range(len(points))) if indices is None else list(indices)
    if any(not isinstance(i, int) or isinstance(i, bool) or i < 0 or i >= len(points) for i in selected):
        raise ValueError('point indices must be integers in the grid')
    if len(set(selected)) != len(selected):
        raise ValueError('duplicate point index')
    checkpoint = output.with_suffix('.jsonl')
    if not checkpoint.resolve().is_relative_to(build):
        raise ValueError('checkpoint must be under repository .test-build')
    if not resume and (output.exists() or checkpoint.exists()):
        raise FileExistsError('report or checkpoint already exists; use resume')
    header = {'kind': 'header', 'schema': 1, 'units': 'm', 'seed': seed, 'trials': trials, 'starts': starts,
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'numpy_version': np.__version__,
              'grid_sha256': hashlib.sha256(json.dumps(points, sort_keys=True, separators=(',', ':')).encode()).hexdigest()}
    rows = {}
    if resume:
        if not checkpoint.is_file():
            raise ValueError('resume requires a checkpoint')
        with checkpoint.open(encoding='utf-8') as stream:
            try:
                saved_header = json.loads(next(stream), parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
                if saved_header != header:
                    raise ValueError('checkpoint configuration or source mismatch')
                for line in stream:
                    row = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
                    json.dumps(row, allow_nan=False)
                    index = row['index']
                    if (not isinstance(index, int) or isinstance(index, bool) or index < 0 or index >= len(points)
                            or index in rows or row.get('status') not in ('complete', 'error')
                            or not isinstance(row.get('metrics'), dict) or row['metrics'].get('point') != points[index]
                            or ('total' in row['metrics'] and row['metrics']['total'] != trials)):
                        raise ValueError('invalid or duplicate checkpoint row')
                    rows[index] = row
            except (StopIteration, json.JSONDecodeError, KeyError, TypeError) as error:
                raise ValueError('corrupt checkpoint') from error
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        with checkpoint.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(json.dumps(header, sort_keys=True, allow_nan=False) + '\n')
            stream.flush()
    try:
        with checkpoint.open('a', encoding='utf-8', newline='\n') as stream:
            for index in selected:
                if index in rows:
                    continue
                unexpected = None
                try:
                    metrics = evaluate_point(points[index], trials, seed, starts=starts)
                    status = 'error' if metrics.get('evaluation_error') else 'complete'
                    row = {'index': index, 'status': status, 'metrics': metrics}
                    serialized = json.dumps(row, sort_keys=True, allow_nan=False)
                except Exception as error:
                    unexpected = error
                    row = {'index': index, 'status': 'error',
                           'metrics': {'point': points[index], 'evaluation_error': f'{type(error).__name__}: {error}'}}
                    serialized = json.dumps(row, sort_keys=True, allow_nan=False)
                stream.write(serialized + '\n')
                stream.flush()
                rows[index] = row
                print(f"point {index}: {row['status']} ({len(rows)}/{len(points)})", flush=True)
                if unexpected is not None:
                    raise unexpected
    finally:
        completed = sum(row['status'] == 'complete' for row in rows.values())
        errors = len(rows) - completed
        report = {'header': header, 'exploratory': trials < 2000, 'decision': None,
                  'complete': completed == len(points), 'completed': completed, 'errors': errors,
                  'pending': len(points)-len(rows),
                  'points': [rows.get(p['index'], {'index': p['index'], 'status': 'pending', 'metrics': None}) |
                             {'settings': p} for p in points]}
        serialized_report = json.dumps(report, sort_keys=True, allow_nan=False, separators=(',', ':')) + '\n'
        temporary = None
        try:
            with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=output.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(serialized_report)
            os.replace(temporary, output)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return report


def aligned_xy_errors(xy, truth_xy):
    """Score after removing translation, rotation and reflection; never used in fitting."""
    xy, truth = np.asarray(xy, dtype=float), np.asarray(truth_xy, dtype=float)
    if xy.ndim != 2 or xy.shape[1] != 2 or xy.shape != truth.shape or not np.all(np.isfinite(xy)) or not np.all(np.isfinite(truth)):
        raise ValueError('xy and truth_xy must be matching finite N by 2 arrays')
    xy, truth = xy - xy[0], truth - truth[0]
    u, _, vt = np.linalg.svd(xy.T @ truth)
    return np.linalg.norm(xy @ u @ vt - truth, axis=1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--trials', type=int, default=2000)
    parser.add_argument('--seed', type=int, default=20261112)
    parser.add_argument('--starts', type=int, default=12)
    parser.add_argument('--point', action='append', type=int, dest='indices')
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    run_sweep(args.output, args.trials, args.seed, args.starts, args.indices, args.resume)
