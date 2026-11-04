"""Compare target3/target9 using the production scheduler on consecutive single-direction records.

No reciprocal directions or geometry matrices are invented from public datasets.
Input data and generated per-record reports remain in ignored directories.
"""
from __future__ import annotations
import csv
import itertools
import json
import math
import os
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
import ble6_research_replay as replay

ROOT = replay.ROOT
BUILD = ROOT / '.test-build/ble6-series'


def windows(rows):
    result, pending, last, previous = [], [], {}, None
    dropped = 0
    for row in rows:
        key = tuple(row.get(k) for k in ('dataset', 'file', 'env', 'setup', 'run_idx'))
        index = row['row']
        if key in last and index <= last[key]:
            raise ValueError(f'Non-increasing source order: {key}/{index}')
        contiguous = key == previous and index == last.get(key, index - 1) + 1
        if not contiguous:
            dropped += len(pending)
            pending = []
        pending.append(row)
        if len(pending) == 10:
            result.append(pending)
            pending = []
        last[key], previous = index, key
    return result, dropped + len(pending)


def native_windows(samples, workdir):
    workdir.mkdir(parents=True, exist_ok=True)
    input_path, output_path = workdir / 'input.bin', workdir / 'output.csv'
    with input_path.open('wb') as stream:
        stream.write(struct.pack('<I', len(samples)))
        for window in samples:
            if len(window) != 10:
                raise ValueError('Each matched window must contain ten raw attempts')
            for mm, confidence in window:
                if not -1 <= mm <= 2147483647 or not 0 <= confidence <= 1000:
                    raise ValueError('Invalid distance/confidence')
                stream.write(struct.pack('<iH', mm, confidence))
    manifest = {'version': 1, 'tests': [{'name': 'ble6_series_replay',
        'sources': ['tests/host/ble6_series_replay.c', 'nRF54L15/src/nrf_cs_scheduler.c'],
        'include_dirs': ['nRF54L15/include']}]}
    manifest_path = workdir / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
    env = {**os.environ, 'BLE6_SERIES_INPUT': str(input_path), 'BLE6_SERIES_OUTPUT': str(output_path)}
    run = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest',
        str(manifest_path.relative_to(ROOT)), '--build-dir', str((workdir / 'native').relative_to(ROOT))],
        cwd=ROOT, env=env, capture_output=True)
    (workdir / 'run.log').write_bytes(run.stdout + run.stderr)
    if run.returncode:
        raise RuntimeError(f'Native series replay failed: {workdir / "run.log"}')
    with output_path.open(newline='', encoding='utf-8') as stream:
        result = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(stream)]
    assert len(result) == 2 * len(samples)
    for i, row in enumerate(result):
        assert row['window'] == i // 2 and row['target'] == (3 if i % 2 == 0 else 9)
    return result


def metrics(errors):
    absolute = [abs(e) for e in errors]
    n = len(errors)
    return {'n': n, 'bias_m': sum(errors) / n if n else None,
        'mae_m': sum(absolute) / n if n else None, 'rmse_m': math.sqrt(sum(e*e for e in errors) / n) if n else None,
        **{f'p{q}_m': replay.percentile(absolute, q / 100) for q in (50, 90, 95, 99)},
        'max_m': max(absolute, default=None),
        **{f'gt_{q}m': sum(e > q for e in absolute) for q in (.3, .5, 1, 2)}}


def native_timing(workdir):
    workdir.mkdir(parents=True, exist_ok=True)
    entry = json.loads((ROOT / 'tests/host/manifest.d/ble-cs-acceptance.json').read_text())['tests'][0]
    entry['name'] = 'ble6_adaptive_timing'
    entry['sources'] = [s.replace('tests/host/nrf_cs_cycle_geometry_golden_test.c',
                                  'tests/host/ble6_adaptive_timing.c') for s in entry['sources']]
    manifest = workdir / 'manifest.json'
    manifest.write_text(json.dumps({'version': 1, 'tests': [entry]}), encoding='utf-8')
    output = workdir / 'output.csv'
    run = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest', str(manifest.relative_to(ROOT)),
        '--build-dir', str((workdir / 'native').relative_to(ROOT))], cwd=ROOT,
        env={**os.environ, 'BLE6_TIMING_OUTPUT': str(output)}, capture_output=True)
    (workdir / 'run.log').write_bytes(run.stdout + run.stderr)
    if run.returncode:
        raise RuntimeError(f'Timing replay failed: {workdir / "run.log"}')
    with output.open(newline='', encoding='utf-8') as stream:
        return {r['mode']: {k: int(v) for k, v in r.items() if k != 'mode'} for r in csv.DictReader(stream)}


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owl-limit', type=int, default=5000, help='First this many usable original rows per OWL file; zero excludes OWL')
    args = parser.parse_args()
    if args.owl_limit < 0:
        parser.error('owl-limit must be nonnegative')
    cases = list(replay.ocm_cases()) + list(replay.wieme_cases())
    if args.owl_limit:
        for name in ('owl-indoor.parquet', 'owl-outdoor.parquet'):
            cases.extend(itertools.islice(replay.owl_cases(replay.OWL / name, stride=1), args.owl_limit))
    results, source = replay.build_and_replay(cases, workdir=BUILD / 'iq-replay')
    extra = replay.confidence_extra(BUILD / 'iq-replay')
    assert len(cases) == len(results) == len(extra)
    enriched = []
    for case, values, quality in zip(cases, results, extra):
        valid = math.isfinite(values[1]) and 0 <= values[1] <= 2147483 and sum(c > 0 for c in case['counts']) >= 15
        enriched.append({k: v for k, v in case.items() if k in
            ('dataset', 'file', 'row', 'env', 'setup', 'run_idx', 'truth_m', 'reference')} |
            {'mm': int(values[1] * 1000 + .5) if valid else -1, 'confidence': int(quality[2]) if valid else 0})
    matched, dropped = windows(enriched)
    summaries = native_windows([[(c['mm'], c['confidence']) for c in w] for w in matched], BUILD / 'series')
    grouped = defaultdict(list)
    details = []
    for i, window in enumerate(matched):
        first = window[0]
        category = 'NLOS' if 'NLOS' in first['file'].split('/') else 'LOS' if 'LOS' in first['file'].split('/') else 'other'
        group = f"{first['dataset']}:{category}"
        pair = summaries[2*i:2*i+2]
        for s in pair:
            s['truth_m'] = window[s['attempts'] - 1]['truth_m']
            s['error_m'] = s['distance_mm'] / 1000 - s['truth_m'] if s['valid'] else None
        row = {'dataset': first['dataset'], 'file': first['file'], 'first_row': first['row'],
            'last_row': window[-1]['row'], 'modes': pair}
        grouped[group].append(row)
        details.append(row)
    report = {'inputs': len(cases), 'windows': len(matched), 'rows_not_in_complete_windows': dropped,
        'owl_limit': args.owl_limit, 'sdk_source': str(source), 'groups': {}}
    for name, rows in grouped.items():
        modes = {}
        for index, target in enumerate((3, 9)):
            all_rows = [r['modes'][index] for r in rows]
            accepted = [r for r in all_rows if r['valid']]
            common = [r['modes'][index]['error_m'] for r in rows if all(s['valid'] for s in r['modes'])]
            modes[str(target)] = {'windows': len(rows), 'accepted': len(accepted), 'coverage': len(accepted) / len(rows),
                'mean_attempts': sum(r['attempts'] for r in all_rows) / len(all_rows),
                'partial_target': sum(r['samples'] < target for r in accepted),
                'accepted_errors': metrics([r['error_m'] for r in accepted]), 'matched_errors': metrics(common),
                'minimum_spread_wrong_gt2m': sum(r['spread_mm'] == 33 and abs(r['error_m']) > 2 for r in accepted)}
        report['groups'][name] = modes
    report['timing'] = native_timing(BUILD / 'timing')
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (BUILD / 'windows.json').write_text(json.dumps(details, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
