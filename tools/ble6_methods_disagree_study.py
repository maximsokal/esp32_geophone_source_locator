"""Summarize legacy-to-current methods-disagree effects on replayed measurements."""
from __future__ import annotations

import math
import json
from pathlib import Path
import struct

import ble6_research_replay as replay


def _errors(values):
    absolute = [abs(value) for value in values]
    return {'n': len(values), 'mae_m': sum(absolute)/len(absolute) if absolute else None,
            'good_le_1m': sum(value <= 1 for value in absolute),
            'bad_gt_2m': sum(value > 2 for value in absolute)}


def summarize(cases, raw4f, legacy4f, firmware5i):
    """Count gap, reported cap and lost acceptance; score raw and calibrated errors separately."""
    if len(cases) != len(raw4f) or len(cases) != len(legacy4f) or len(cases) != len(firmware5i):
        raise ValueError('case and replay record counts must match')
    groups = {}
    for case, raw, legacy, firmware in zip(cases, raw4f, legacy4f, firmware5i, strict=True):
        group = groups.setdefault(case['dataset'], {'total': 0, 'raw_gap_gt_1m': 0,
            'methods_disagree_reason': 0, 'lost_acceptance': 0, '_raw_errors': [], '_calibrated_errors': []})
        group['total'] += 1
        distance, confidence, reason, method, valid = firmware
        if method == 1 and math.isfinite(raw[1]) and raw[1] >= 0 and math.isfinite(raw[2]) and raw[2] >= 0 and abs(raw[1]-raw[2]) > 1:
            group['raw_gap_gt_1m'] += 1
        if reason == 11:
            group['methods_disagree_reason'] += 1
        if valid and method == 1 and reason == 11 and legacy[2] >= 500 and confidence < 500:
            group['lost_acceptance'] += 1
            truth = case.get('truth_m')
            if truth is not None and math.isfinite(truth):
                if math.isfinite(raw[1]):
                    group['_raw_errors'].append(raw[1]-truth)
                group['_calibrated_errors'].append(distance/1000-truth)
    for group in groups.values():
        group['lost_raw_error'] = _errors(group.pop('_raw_errors'))
        group['lost_calibrated_error'] = _errors(group.pop('_calibrated_errors'))
    return groups


def validate_report(report, source):
    """Reject legacy reports and reports made by a different replay source."""
    if 'with_methods' not in report.get('confidence_policy', ''):
        raise ValueError('report does not use with_methods policy')
    hashes = report.get('sha256', {})
    recorded = [value for name, value in hashes.items()
                if name.replace('\\', '/').endswith('tools/ble6_research_replay.py')]
    if len(recorded) != 1 or recorded[0] != replay.sha256(source):
        raise ValueError('replay source hash mismatch')
    if not isinstance(report.get('replayed'), int) or report['replayed'] < 0:
        raise ValueError('invalid replayed count')
    if not isinstance(report.get('owl_stride'), int) or report['owl_stride'] < 1:
        raise ValueError('invalid OWL stride')


def main():
    source = Path(replay.__file__).resolve()
    work = replay.BUILD
    original_report = work / 'report.json'
    report = json.loads(original_report.read_text(encoding='utf-8'))
    validate_report(report, source)
    cases = list(replay.ocm_cases()) + list(replay.wieme_cases()) + list(replay.waves_cases())
    for name in ('owl-indoor.parquet', 'owl-outdoor.parquet'):
        cases.extend(replay.owl_cases(replay.OWL / name, report['owl_stride']))
    raw = list(struct.iter_unpack('<4f', (work / 'output.bin').read_bytes()))
    legacy = replay.confidence_extra(work)
    firmware = replay.firmware_results(work)
    if any(len(rows) != report['replayed'] for rows in (cases, raw, legacy, firmware)):
        raise ValueError('report replayed count and source record counts do not match')
    paths = [original_report, work/'output.bin', work/'extra.bin', work/'firmware.bin', source, Path(__file__).resolve()]
    result = {'replayed': report['replayed'], 'owl_stride': report['owl_stride'],
              'groups': summarize(cases, raw, legacy, firmware),
              'sha256': {str(path): replay.sha256(path) for path in paths},
              'limitations': ['host FFT, not target RF', 'all 75 channels assumed planned',
                              '1800 mm calibration is not established for third-party hardware',
                              'no new bench logs']}
    output = replay.ROOT / '.test-build/ble6-methods-disagree/report.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(f'REPORT {output}: replayed={len(cases)}')


if __name__ == '__main__':
    main()
