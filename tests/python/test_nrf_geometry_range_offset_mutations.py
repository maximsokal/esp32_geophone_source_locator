"""Compile isolated mutants and require a host-test assertion to reject each one."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'components/nrf_geometry_solver/nrf_geometry_range_offset.c'
BUILD = ROOT / '.test-build/offset-mutations'

MUTANTS = {
    'estimate_sign': (
        '(int64_t)pair->automatic_distance_mm - pair->distance_mm',
        '(int64_t)pair->distance_mm - pair->automatic_distance_mm',
    ),
    'mean_for_median': (
        'result.offset_mm = (int32_t)(count % 2 ? offsets[count / 2] :\n'
        '                                     (offsets[count / 2 - 1] + offsets[count / 2]) / 2);',
        'int64_t sum = 0;\n'
        '        for (size_t j = 0; j < count; ++j) sum += offsets[j];\n'
        '        result.offset_mm = (int32_t)(sum / (int64_t)count);',
    ),
    'bypass_ifft': (
        'return (pair->provider_quality_flags & NRF_CS_PROVIDER_METHOD_MASK) == NRF_CS_PROVIDER_METHOD_IFFT;',
        '(void)pair; return true;',
    ),
    'ignore_estimate_state': (
        'pair->state != NRF_CS_PAIR_VALID || !(pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL)',
        '!(pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL)',
    ),
    'application_sign': (
        '(int64_t)pair->distance_mm - offset_mm',
        '(int64_t)pair->distance_mm + offset_mm',
    ),
    'ignore_height': ('corrected < dz', 'corrected < 0 * dz'),
    'ignore_single_direction': ('pair->single_direction || ', ''),
    'ignore_direction_disagreement': (
        'pair->direction_disagreement_mm > NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM', 'false'),
}


def run_case(name, replacement=None):
    work = BUILD / name
    work.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text(encoding='utf-8-sig')
    if replacement is not None:
        old, new = replacement
        assert source.count(old) == 1, f'{name}: expected one mutation site'
        source = source.replace(old, new, 1)
    source_path = work / 'nrf_geometry_range_offset.c'
    source_path.write_text(source, encoding='utf-8')
    manifest = work / 'manifest.json'
    manifest.write_text(json.dumps({'version': 1, 'tests': [{
        'name': 'nrf_geometry_range_offset',
        'sources': [source_path.relative_to(ROOT).as_posix(),
                    'tests/host/nrf_geometry_range_offset_test.c'],
        'include_dirs': ['components/nrf_geometry_solver/include', 'nRF54L15/include'],
        'defines': ['NRF_GEOMETRY_SOLVER_HOST_TEST=1'],
    }]}), encoding='utf-8')
    build = work / 'build'
    report = build / 'host-test-report.json'
    report.unlink(missing_ok=True)
    result = subprocess.run([sys.executable, 'tools/run_host_tests.py', '--manifest', str(manifest),
                             '--build-dir', str(build)], cwd=ROOT, capture_output=True, text=True)
    assert report.is_file(), f'{name}: no report; runner exit {result.returncode}\n{result.stdout}\n{result.stderr}'
    entries = json.loads(report.read_text(encoding='utf-8'))['results']
    assert len(entries) == 1 and entries[0]['phase'] == 'run', (name, entries)
    assert 'exit_code' in entries[0], (name, entries)
    return result.returncode, entries[0]


def test_baseline_passes():
    code, result = run_case('baseline')
    assert code == 0 and result['status'] == 'PASS' and result['exit_code'] == 0, result


@pytest.mark.parametrize('name', MUTANTS)
def test_mutant_is_killed_by_host_test(name):
    code, result = run_case(name, MUTANTS[name])
    assert code == 1 and result['status'] == 'FAIL' and result['exit_code'] != 0, result
