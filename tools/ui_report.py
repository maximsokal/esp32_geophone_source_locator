#!/usr/bin/env python3
"""Summarize existing Playwright/JUnit results and native run.json. Never runs tests."""
from __future__ import annotations

import argparse
import base64
from collections import Counter
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def source_identity(root: Path) -> dict:
    """Include dirty and new UI/test/config inputs; exclude output and dependencies."""
    paths = []
    for directory in ('components', 'main', 'webui/src', 'webui/tests', 'webui/scripts', 'tests/ui_render',
                      'tests/python', 'tools', '.github/workflows'):
        paths.extend(p for p in (root / directory).rglob('*') if p.is_file() and
                     '__pycache__' not in p.parts and p.suffix in {
                         '.c', '.h', '.cpp', '.py', '.ts', '.tsx', '.json', '.yml', '.css', '.png', '.txt', '.js', '.mjs', '.html', '.cmake'})
    paths.extend(root / p for p in ('dependencies.lock', 'sdkconfig', 'sdkconfig.defaults',
                 'webui/package.json', 'webui/package-lock.json', 'webui/index.html',
                 'webui/playwright.config.ts', 'webui/playwright.layout.config.ts', 'webui/playwright.visual.config.mjs', 'tests/ui_contract.json',
                 'webui/tsconfig.e2e.json', 'webui/vite.config.ts',
                 'webui/tsconfig.json', 'webui/vitest.dom.config.ts', 'webui/tsconfig.test.json', 'sgconfig.yml'))
    digest = hashlib.sha256()
    paths = sorted({p for p in paths if p.is_file()})
    for path in paths:
        digest.update(path.relative_to(root).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    return {'head': head, 'source_sha256': digest.hexdigest(), 'files': len(paths)}


def initialize(output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    identity = {'started_at': utc_now(), **source_identity(ROOT),
                'environment': {'platform': platform.platform(), 'python': platform.python_version()}}
    (output / 'identity.json').write_text(json.dumps(identity, indent=2), encoding='utf-8')
    return identity


def record(subsystem: str, test_id: str, status: str, actual: str, **fields) -> dict:
    return {'id': test_id, 'subsystem': subsystem, 'screen': None, 'state': None,
            'viewport': '320x240' if subsystem == 'native' else '393x852',
            'category': 'assertion', 'expected': 'All selected assertions pass',
            'actual': ANSI.sub('', actual), 'status': status, 'source': None,
            'artifacts': {}, 'root_cause': None, **fields}


def read_json(path: Path | None) -> dict:
    if path is None:
        raise ValueError('Report input was not supplied')
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError(f'Expected a JSON object: {path}')
    return value


def check_fresh(path: Path, identity: dict | None) -> None:
    if identity is not None and path.stat().st_mtime + 0.01 < datetime.fromisoformat(identity['started_at']).timestamp():
        raise ValueError(f'Stale report predates this run: {path}')


def web_records(path: Path, identity: dict | None = None, subsystem: str = 'web') -> list[dict]:
    check_fresh(path, identity)
    report = read_json(path)
    # stats/errors/suites are required: a partial or empty JSON file is not a pass.
    started = datetime.fromisoformat(report['stats']['startTime'].replace('Z', '+00:00'))
    if identity is not None and started.timestamp() < datetime.fromisoformat(identity['started_at']).timestamp():
        raise ValueError('Playwright report belongs to an earlier run')
    rows = []
    for error in report['errors']:
        rows.append(record(subsystem, 'playwright/startup', 'BLOCKED', error.get('message', str(error)), category='infrastructure'))

    def visit(suites, parents):
        for suite in suites:
            names = [*parents, suite.get('title', '')]
            for spec in suite.get('specs', []):
                for test in spec['tests']:
                    results = test['results']
                    if test['status'] not in ('expected', 'unexpected', 'flaky', 'skipped'):
                        raise ValueError('Unknown Playwright test status')
                    errors = [e.get('message', str(e)) for result in results for e in result.get('errors', [])]
                    actual = '\n'.join(errors) or test['status']
                    statuses = [result['status'] for result in results]
                    if test['status'] == 'flaky' or len(results) > 1:
                        status = 'FLAKY/UNSTABLE'
                    elif not results or all(s == 'skipped' for s in statuses):
                        status = 'SKIPPED'
                    elif test.get('expectedStatus', 'passed') != 'passed':
                        status = 'SKIPPED'  # expected failures are not proof of working product behavior
                    else:
                        status = 'PASS' if test['status'] == 'expected' and statuses == ['passed'] else 'FAIL'
                    if any(token in actual for token in ('Executable doesn\'t exist', 'browserType.launch:', 'browser has been closed')):
                        status = 'BLOCKED'
                    attachments = [a for result in results for a in result.get('attachments', [])]
                    metadata = report['config'].get('metadata', {})
                    visual_mode = metadata.get('visualMode')
                    if subsystem == 'visual' and visual_mode not in ('capture', 'compare'):
                        raise ValueError('Visual report has no valid capture/compare mode')
                    if visual_mode == 'capture' and status == 'PASS':
                        status = 'CAPTURED_NOT_APPROVED'
                        actual = 'Actual PNG captured; no approved reference comparison'
                    visual_artifacts = {}
                    if visual_mode in ('capture', 'compare'):
                        for name in ('candidate.json', 'environment.json', 'stability.json', 'execution.json'):
                            if (path.parent / name).is_file():
                                visual_artifacts[name] = str(path.parent / name)
                    environment = dict(metadata)
                    viewport = metadata.get('viewport')
                    for attachment in attachments:
                        if attachment.get('name') != 'runtime':
                            continue
                        runtime = (json.loads(base64.b64decode(attachment['body'], validate=True))
                                   if 'body' in attachment else read_json(Path(attachment['path'])))
                        size = runtime.get('viewport')
                        if not isinstance(size, dict) or any(type(size.get(k)) is not int or size[k] <= 0 for k in ('width', 'height')):
                            raise ValueError('Invalid browser runtime viewport')
                        viewport = f"{size['width']}x{size['height']}"
                        environment.update(browser=runtime.get('browser'), locale=runtime.get('locale'))
                    rows.append(record(subsystem, ' / '.join([*names, spec['title'], test['projectName']]), status, actual,
                        state=' / '.join(names), source={'file': spec['file'], 'line': spec['line']},
                        viewport=viewport, environment=environment,
                        artifacts={'attachments': attachments, 'playwright_json': str(path),
                                   'html': str(path.parent / 'html/index.html'), **visual_artifacts},
                        reproduce=metadata.get('reproduce', 'cd webui; npm run test:ui:smoke')))
            visit(suite.get('suites', []), names)
    visit(report['suites'], [])
    if not rows:
        raise ValueError('Playwright selected/executed no tests')
    stats_count = sum(report['stats'][key] for key in ('expected', 'unexpected', 'skipped', 'flaky'))
    if stats_count != sum(row['category'] == 'assertion' for row in rows):
        raise ValueError('Playwright test count does not match its summary')
    expected_count = report['config'].get('metadata', {}).get('expectedTests')
    if expected_count is not None and stats_count != expected_count:
        rows.append(record(subsystem, f'{subsystem}/selection', 'BLOCKED',
            f'Expected {expected_count} selected tests, found {stats_count}', category='coverage'))
    return rows


def junit_records(junit: Path, subsystem: str, identity: dict | None = None) -> list[dict]:
    check_fresh(junit, identity)
    tree = ET.parse(junit)
    rows = []
    for case in tree.iter('testcase'):
        failure, error, skipped = case.find('failure'), case.find('error'), case.find('skipped')
        status, detail = ('FAIL', failure) if failure is not None else (
            ('BLOCKED', error) if error is not None else (('SKIPPED', skipped) if skipped is not None else ('PASS', None)))
        rows.append(record(subsystem, f"{case.get('classname')}.{case.get('name')}", status,
            ''.join(detail.itertext()) if detail is not None else 'passed',
            source={'file': case.get('file') or case.get('classname'), 'line': case.get('line')},
            artifacts={'junit': str(junit)},
            viewport='320x240' if subsystem == 'native' else None,
            environment={'runtime': {'native': 'Native LVGL/runner', 'components': 'jsdom', 'domain': 'Node'}[subsystem]},
            reproduce=('python -m pytest tests/python/test_ui_render_runner.py tests/python/test_ui_render.py -q'
                       if subsystem == 'native' else f'cd webui; npm run test:{subsystem}')))
    if not rows:
        raise ValueError(f'{subsystem} JUnit contains no tests')
    for attribute, actual in (('tests', len(rows)), ('failures', sum(row['status'] == 'FAIL' for row in rows)),
                              ('errors', sum(row['status'] == 'BLOCKED' for row in rows))):
        declared = tree.getroot().get(attribute)
        if declared is not None and int(declared) != actual:
            raise ValueError(f'{subsystem} JUnit {attribute} count contradicts its cases')
    return rows


def native_records(junit: Path, native_root: Path, identity: dict | None = None) -> list[dict]:
    rows = junit_records(junit, 'native', identity)
    records = list(native_root.glob('real-lvgl*/run.json'))
    if len(records) != 1:
        rows.append(record('native', 'native/capture', 'BLOCKED',
            f'Expected one main real-LVGL run.json, found {len(records)}; see pytest setup errors',
            category='infrastructure', artifacts={'junit': str(junit)}))
        return rows
    run_path = records[0]
    check_fresh(run_path, identity)
    run = read_json(run_path)
    if run['status'] not in ('PASS', 'FAIL'):
        raise ValueError('Native capture did not finish')
    if not isinstance(run['frames'], int) or run['frames'] <= 0:
        raise ValueError('Native capture has no frames')
    comparisons = run['comparisons']
    if any(not isinstance(comparisons[key], int) or comparisons[key] < 0
           for key in ('matched', 'missing', 'different')):
        raise ValueError('Invalid native comparison counts')
    if run['status'] == 'PASS' and (comparisons['missing'] or comparisons['different'] or not comparisons['matched']):
        raise ValueError('Native PASS contradicts its reference comparison counts')
    rows.append(record('native', 'native/capture', run['status'], json.dumps({
        'frames': run.get('frames'), 'comparisons': run.get('comparisons'), 'error': run.get('error')}),
        category='render/reference', environment=run,
        artifacts={'metadata': str(run_path), 'gallery': str(run_path.parent / 'index.html')}))
    return rows


def summarize(*, web: Path | None, native_junit: Path | None, native_root: Path | None,
              only: str = 'both', identity: dict | None = None,
              exit_codes: dict | None = None, components_junit: Path | None = None,
              domain_junit: Path | None = None, layout: Path | None = None, visual: Path | None = None) -> dict:
    rows = []
    selected = ['web', 'native'] if only == 'both' else [only]
    extra_junit = {'components': components_junit, 'domain': domain_junit}
    selected.extend(name for name, path in extra_junit.items() if path is not None and name not in selected)
    if layout is not None and 'layout' not in selected:
        selected.append('layout')
    if visual is not None and 'visual' not in selected:
        selected.append('visual')
    for subsystem in selected:
        try:
            if subsystem in ('web', 'layout', 'visual'):
                path = visual if subsystem == 'visual' else layout if subsystem == 'layout' else web
                if path is None:
                    raise ValueError('Playwright JSON was not supplied')
                found = web_records(path, identity, subsystem)
            elif subsystem == 'native':
                if native_junit is None or native_root is None:
                    raise ValueError('Native JUnit/root were not supplied')
                found = native_records(native_junit, native_root, identity)
            else:
                path = extra_junit[subsystem]
                if path is None:
                    raise ValueError(f'{subsystem} JUnit was not supplied')
                found = junit_records(path, subsystem, identity)
            rows.extend(found)
            code = (exit_codes or {}).get(subsystem)
            if code not in (None, 0) and not any(row['status'] in ('FAIL', 'BLOCKED') for row in found):
                rows.append(record(subsystem, f'{subsystem}/process', 'BLOCKED',
                                   f'Process exited {code}; report cannot override its failure', category='infrastructure'))
        except (OSError, ValueError, KeyError, TypeError, AttributeError, ET.ParseError) as exc:
            rows.append(record(subsystem, f'{subsystem}/report-input', 'BLOCKED', str(exc), category='infrastructure'))
    counts = dict(Counter(row['status'] for row in rows))
    status = next((s for s in ('FAIL', 'BLOCKED', 'FLAKY/UNSTABLE', 'SKIPPED', 'CAPTURED_NOT_APPROVED') if counts.get(s)), 'PASS')
    return {'status': status, 'counts': counts, 'selected': selected, 'identity': identity,
            'generated_at': utc_now(), 'tests': rows,
            'not_run': ['Physical TFT/Android Chrome/immersive XR/HIL',
                        'Non-React phone pages and graphics lifecycle (later slices)',
                        *([] if 'visual' in selected else ['Approved visual comparison not selected; separate reference approval required']),
                        *([] if 'layout' in selected else ['Mobile geometry/accessibility not selected'])]}


def write_report(output: Path, report: dict) -> None:
    output.mkdir(parents=True, exist_ok=True)
    (output / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    failures = [row for row in report['tests'] if row['status'] != 'PASS']
    priority = {'FAIL': 0, 'BLOCKED': 1, 'FLAKY/UNSTABLE': 2, 'SKIPPED': 3, 'CAPTURED_NOT_APPROVED': 4}
    failures.sort(key=lambda row: priority.get(row['status'], 4))
    lines = [f"UI_RESULT: {report['status']} | " + ', '.join(f'{s}={n}' for s, n in report['counts'].items())]
    for row in failures[:3]:
        lines.append(f"{row['id']}: {row['status']} {row['actual'][:300]}")
    lines.append(f"Details: {output / 'index.html'}")
    (output / 'summary.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    def escape(value):
        return html.escape(str(value))
    def link(path, preview=False):
        target = os.path.relpath(Path(path).resolve(), output.resolve()).replace('\\', '/')
        label = (f'<img loading="lazy" src="{escape(target)}" alt="{escape(Path(path).name)}">'
                 if preview else escape(Path(path).name))
        return f'<a href="{escape(target)}">{label}</a>'
    table = []
    for row in report['tests']:
        links = [link(value) for value in row['artifacts'].values() if isinstance(value, str)]
        for attachment in row['artifacts'].get('attachments', []):
            if attachment.get('path'):
                links.append(link(attachment['path'], preview=attachment.get('name') == 'visual-actual'))
        table.append(f"<tr><td>{escape(row['status'])}</td><td>{escape(row['id'])}</td>"
                     f"<td><details><summary>Actual / source</summary><pre>{escape(row['actual'])}</pre>"
                     f"<pre>{escape(row['source'])}</pre></details></td><td>{' | '.join(links)}</td></tr>")
    (output / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>UI regression</title>'
        '<style>body{font:16px system-ui;margin:24px;background:#fafafa}td,th{border:1px solid #ccc;padding:8px;vertical-align:top}'
        'table{border-collapse:collapse;width:100%}pre{white-space:pre-wrap;overflow-wrap:anywhere}td:nth-child(2){overflow-wrap:anywhere}'
        'img{display:block;width:100%;max-width:320px;max-height:420px;object-fit:contain;object-position:top;background:#111}</style>'
        f'<h1>{escape(lines[0])}</h1><p>{escape(report["generated_at"])}</p><p>Selected: {escape(report["selected"])}</p>'
        f'<details><summary>Run identity / evidence</summary><pre>{escape(json.dumps(report["identity"], indent=2))}</pre></details>'
        '<p>Not run: ' + escape('; '.join(report['not_run'])) + '</p><p>' + link(output / 'summary.json') +
        (' | ' + link(output / 'execution.json') if (output / 'execution.json').is_file() else '') +
        (' | ' + link(output / 'native-build/build.log') if (output / 'native-build/build.log').is_file() else '') + '</p>'
        '<table><tr><th>Status</th><th>Test</th><th>Evidence</th><th>Artifacts</th></tr>' + ''.join(table) + '</table>', encoding='utf-8')
    print('\n'.join(lines))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--init', action='store_true', help='Create a fresh run directory/identity before execution')
    parser.add_argument('--web', type=Path)
    parser.add_argument('--layout', type=Path)
    parser.add_argument('--visual', type=Path)
    parser.add_argument('--native-junit', type=Path)
    parser.add_argument('--native-root', type=Path)
    parser.add_argument('--only', choices=('web', 'native', 'both', 'components', 'domain', 'layout', 'visual'), default='both')
    parser.add_argument('--components-junit', type=Path)
    parser.add_argument('--domain-junit', type=Path)
    parser.add_argument('--identity', type=Path, help='Pre-run identity; rejects stale reports or changed source')
    parser.add_argument('--web-exit-code', type=int)
    parser.add_argument('--layout-exit-code', type=int)
    parser.add_argument('--visual-exit-code', type=int)
    parser.add_argument('--native-exit-code', type=int)
    args = parser.parse_args(argv)
    if args.init:
        initialize(args.output)
        print(f'UI run initialized: {args.output}')
        return 0
    try:
        identity = read_json(args.identity) if args.identity else None
        if identity is not None:
            datetime.fromisoformat(identity['started_at'])
            for key in ('head', 'source_sha256', 'files'):
                if not identity[key]:
                    raise ValueError(f'Invalid run identity: {key}')
        report = summarize(web=args.web, native_junit=args.native_junit, native_root=args.native_root,
                           only=args.only, identity=identity,
                           components_junit=args.components_junit, domain_junit=args.domain_junit, layout=args.layout, visual=args.visual,
                           exit_codes={'web': args.web_exit_code, 'native': args.native_exit_code, 'layout': args.layout_exit_code, 'visual': args.visual_exit_code})
        if identity is not None and source_identity(ROOT) != {key: identity[key] for key in ('head', 'source_sha256', 'files')}:
            report['status'] = 'BLOCKED'
            report['tests'].append(record('run', 'run/source-identity', 'BLOCKED', 'Source changed during execution', category='infrastructure'))
            report['counts'] = dict(Counter(row['status'] for row in report['tests']))
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        report = {'status': 'BLOCKED', 'counts': {'BLOCKED': 1}, 'selected': [args.only],
                  'generated_at': utc_now(), 'identity': None, 'not_run': ['Invalid run identity'],
                  'tests': [record('run', 'run/identity', 'BLOCKED', str(exc), category='infrastructure')]}
    write_report(args.output, report)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
