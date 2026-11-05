"""Falsify a triangle-based edge selector; research only, never firmware policy."""
from __future__ import annotations

import itertools
import hashlib
import json
import math
import random

import ble6_geometry_consensus as geometry

BUILD = geometry.ROOT / '.test-build/ble6-forced-edges-study'
SEEDS = (110420270, 110420271, 110420272)


def choose(case, result):
    baseline = result['baseline']
    if baseline['ok']:
        return {'accepted': True, 'reason': 'baseline', 'mask': 0, 'solution': baseline}
    if len(case['pairs']) != 10 or any(p['state'] != 2 for p in case['pairs']):
        return {'accepted': False, 'reason': 'incomplete'}
    forced = [i for i, count in enumerate(result['violations']) if count == 3]
    edges = list(itertools.combinations(range(5), 2))
    if len(forced) != 2 or set(edges[forced[0]]) & set(edges[forced[1]]):
        return {'accepted': False, 'reason': 'not_two_disjoint_forced'}
    mask = sum(1 << p for p in forced)
    if any(case['pairs'][p]['flags'] & 64 for p in forced):
        return {'accepted': False, 'reason': 'manual'}
    candidates = [s for s in result['solutions'] if s['mask'] == mask]
    if {s['order'] for s in candidates} != set(range(24)):
        return {'accepted': False, 'reason': 'not_all_orders'}
    if geometry.shape_disagreement(candidates) > geometry.TOLERANCE:
        return {'accepted': False, 'reason': 'shape_disagreement'}
    best = min(candidates, key=lambda s: (s['residual_mm'], s['order']))
    return {'accepted': True, 'reason': 'recovered', 'mask': mask, 'solution': best}


def controls():
    good = geometry.make_case('two-gross', geometry.REFERENCE, {0: 12000, 9: 12000})
    # The remaining eight edges describe a different physical layout exactly.
    truth = [[0, 0, 0], [-378, 3597, 62], [-3593, -1583, 196],
             [3911, -2554, 54], [-4414, -3563, -35]]
    shifted = [p[:] for p in truth]
    shifted[1][0] += 2000
    wrong = geometry.make_case('coherent-plus-two-gross', shifted, {1: 12000, 6: 12000})
    wrong['truth'] = truth
    original = geometry.make_case('original', truth)
    wrong['bad_mask'] = sum(1 << i for i, (a, b) in enumerate(zip(wrong['pairs'], original['pairs']))
                            if a['distance'] != b['distance'])
    for case in (good, wrong):
        case['class'] = case['name']
    return [good, wrong]


def fresh_cases():
    cases = controls()
    for seed in SEEDS:
        rng = random.Random(seed)
        for layout in range(8):
            points = [[0, 0, 0]]
            while len(points) < 5:
                p = [rng.randrange(-5000, 5001), rng.randrange(-5000, 5001), rng.randrange(-300, 301)]
                if all(math.dist(p, q) > 1500 for q in points):
                    points.append(p)
            edges = list(itertools.combinations(range(5), 2))
            for p, q in itertools.combinations(range(10), 2):
                if set(edges[p]) & set(edges[q]):
                    continue
                for coherent in (False, True):
                    observed = [p[:] for p in points]
                    if coherent:
                        observed[1][0] += 2000
                    case = geometry.make_case(f'{seed}/{layout}/{p}/{q}/{coherent}', observed,
                                              {p: 12000, q: 12000})
                    case['truth'] = points
                    case['class'] = 'coherent_plus_two' if coherent else 'two_gross'
                    clean = geometry.make_case('truth', points)
                    case['bad_mask'] = sum(1 << i for i, (a, b) in enumerate(zip(case['pairs'], clean['pairs']))
                                           if a['distance'] != b['distance'])
                    cases.append(case)
    return cases


def evaluate(cases, results):
    groups, details = {}, []
    for case, result in zip(cases, results):
        decision = choose(case, result)
        row = {'name': case['name'], 'class': case['class'], 'violations': result['violations'],
               'baseline_ok': result['baseline']['ok'],
               **{k: v for k, v in decision.items() if k != 'solution'}}
        baseline_error = (geometry.node_error(result['baseline'], case['truth'])['max_mm']
                          if row['baseline_ok'] else None)
        if decision['accepted']:
            row.update(geometry.node_error(decision['solution'], case['truth']))
            row['false_exclusions'] = (decision['mask'] & ~case['bad_mask']).bit_count()
        for label in ('all', case['class']):
            group = groups.setdefault(label, {'n': 0, 'baseline_accepted': 0, 'accepted': 0,
                'baseline_wrong_gt750': 0, 'wrong_gt750': 0, 'recovered': 0,
                'new_wrong_gt750': 0, 'new_wrong_gt2m': 0, 'false_exclusions': 0,
                'errors_mm': []})
            group['n'] += 1
            group['baseline_accepted'] += row['baseline_ok']
            group['accepted'] += row['accepted']
            group['baseline_wrong_gt750'] += baseline_error is not None and baseline_error > 750
            group['wrong_gt750'] += row['accepted'] and row['max_mm'] > 750
            if row['accepted']:
                group['errors_mm'].append(row['max_mm'])
            group['recovered'] += row['reason'] == 'recovered'
            group['new_wrong_gt750'] += row['reason'] == 'recovered' and row['max_mm'] > 750
            group['new_wrong_gt2m'] += row['reason'] == 'recovered' and row['max_mm'] > 2000
            group['false_exclusions'] += row.get('false_exclusions', 0)
        details.append(row)
    for group in groups.values():
        errors = group.pop('errors_mm')
        group['accepted_max_node_error_p95_mm'] = float(geometry.np.percentile(errors, 95)) if errors else None
    return {'groups': groups, 'details': details}


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    historical = [c for seed, count in ((11042026, 3), (41022026, 8), (8112026, 16))
                  for c in geometry.synthetic_cases(seed, count)]
    fresh = fresh_cases()
    source_paths = ('components/nrf_geometry_solver/nrf_geometry_solver.c',
                    'components/nrf_geometry_solver/nrf_geometry_triangles.c',
                    'tests/host/ble6_geometry_orders.c', 'tools/ble6_forced_edges_study.py')
    report = {'research_only': True, 'seeds': SEEDS, 'decision': 'not_deployed',
              'source_sha256': {p: hashlib.sha256((geometry.ROOT / p).read_bytes()).hexdigest()
                                for p in source_paths}}
    for label, cases in (('historical', historical), ('fresh', fresh)):
        results = geometry.native_solutions(cases, BUILD / label)
        report[label] = evaluate(cases, results)
        report[label]['input_sha256'] = hashlib.sha256((BUILD / label / 'input.txt').read_bytes()).hexdigest()
        report[label]['output_sha256'] = hashlib.sha256((BUILD / label / 'output.csv').read_bytes()).hexdigest()
        print(label, json.dumps(report[label]['groups']), flush=True)
    (BUILD / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
