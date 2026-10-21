import importlib.util
import base64
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ui_report', ROOT / 'tools/ui_report.py')
assert spec and spec.loader
reporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reporter)


def web_report(tmp_path, *, status='passed', expected='passed', overall='expected', errors=None):
    data = {
        'config': {'metadata': {'evidence': 'isolated reporter fixture'}},
        'stats': {'startTime': '2026-01-01T00:00:01Z', 'expected': 1, 'unexpected': 0, 'skipped': 0, 'flaky': 0},
        'errors': [],
        'suites': [{'title': 'navigation', 'specs': [{'title': 'home', 'file': 'navigation.spec.ts', 'line': 8,
                    'tests': [{'projectName': 'chromium', 'status': overall, 'expectedStatus': expected,
                               'results': [{'status': status, 'errors': errors or [], 'attachments': []}]}]}]}],
    }
    if overall != 'expected':
        data['stats']['expected'] = 0
        data['stats'][{'unexpected': 'unexpected', 'flaky': 'flaky', 'skipped': 'skipped'}[overall]] = 1
    path = tmp_path / 'playwright.json'
    path.write_text(json.dumps(data), encoding='utf-8')
    return path, data


def web_summary(path, **kwargs):
    return reporter.summarize(web=path, native_junit=None, native_root=None, only='web', **kwargs)


@pytest.mark.parametrize('fault', ['none', 'assertion', 'process', 'missing'])
def test_layout_evidence_is_required_and_cannot_be_hidden_by_smoke(tmp_path, fault):
    smoke, _ = web_report(tmp_path)
    layout_dir = tmp_path / 'layout'
    layout_dir.mkdir()
    layout, data = web_report(layout_dir, status='failed' if fault == 'assertion' else 'passed',
                              overall='unexpected' if fault == 'assertion' else 'expected')
    data['config']['metadata'].update(viewport='320x568,393x852', reproduce='cd webui; npm run test:ui:layout')
    layout.write_text(json.dumps(data), encoding='utf-8')
    if fault == 'missing':
        layout.unlink()
    result = web_summary(smoke, layout=layout, exit_codes={'layout': 1 if fault == 'process' else 0})
    assert result['selected'] == ['web', 'layout']
    assert result['status'] == {'none': 'PASS', 'assertion': 'FAIL', 'process': 'BLOCKED', 'missing': 'BLOCKED'}[fault]
    row = next(row for row in result['tests'] if row['subsystem'] == 'layout')
    if fault != 'missing':
        assert row['reproduce'] == 'cd webui; npm run test:ui:layout'
        assert row['viewport'] == '320x568,393x852'


def test_pass_has_actual_source_selection_and_evidence(tmp_path):
    path, _ = web_report(tmp_path)
    result = web_summary(path)
    assert result['status'] == 'PASS'
    assert result['selected'] == ['web']
    assert result['tests'][0]['source'] == {'file': 'navigation.spec.ts', 'line': 8}
    assert result['tests'][0]['environment']['evidence'] == 'isolated reporter fixture'
    assert result['tests'][0]['root_cause'] is None
    assert 'Physical' in result['not_run'][0]


@pytest.mark.parametrize('mode,outcome', [('capture', 'CAPTURED_NOT_APPROVED'), ('compare', 'PASS'), ('invalid', 'BLOCKED')])
def test_visual_capture_is_never_reported_as_an_approved_comparison(tmp_path, mode, outcome):
    path, data = web_report(tmp_path)
    data['config']['metadata']['visualMode'] = mode
    actual = tmp_path / 'actual.png'
    actual.write_bytes(b'isolated reporter image fixture')
    data['suites'][0]['specs'][0]['tests'][0]['results'][0]['attachments'].append(
        {'name': 'visual-actual', 'path': str(actual), 'contentType': 'image/png'})
    (tmp_path / 'candidate.json').write_text('{}', encoding='utf-8')
    path.write_text(json.dumps(data), encoding='utf-8')
    result = reporter.summarize(web=None, native_junit=None, native_root=None, visual=path, only='visual')
    assert result['status'] == outcome
    assert result['tests'][0]['status'] == outcome
    code = reporter.main(['--only', 'visual', '--visual', str(path), '--output', str(tmp_path / 'summary')])
    assert code == (0 if outcome == 'PASS' else 1)
    rendered = (tmp_path / 'summary/index.html').read_text(encoding='utf-8')
    if mode != 'invalid':
        assert '<img loading="lazy" src="../actual.png" alt="actual.png">' in rendered
        assert 'href="../candidate.json"' in rendered


def test_failed_visual_capture_and_missing_visual_input_remain_failures(tmp_path):
    path, data = web_report(tmp_path, status='failed', overall='unexpected', errors=[{'message': 'visible text changed'}])
    data['config']['metadata']['visualMode'] = 'capture'
    path.write_text(json.dumps(data), encoding='utf-8')
    assert reporter.summarize(web=None, native_junit=None, native_root=None, visual=path, only='visual')['status'] == 'FAIL'
    path.unlink()
    assert reporter.summarize(web=None, native_junit=None, native_root=None, visual=path, only='visual')['status'] == 'BLOCKED'


@pytest.mark.parametrize('width', [360, -1])
def test_browser_records_use_measured_runtime_viewport_or_block_invalid_evidence(tmp_path, width):
    path, data = web_report(tmp_path)
    runtime = {'browser': 'pinned-chromium', 'locale': 'uk', 'viewport': {'width': width, 'height': 800}}
    attachment = {'name': 'runtime', 'body': base64.b64encode(json.dumps(runtime).encode()).decode()}
    data['suites'][0]['specs'][0]['tests'][0]['results'][0]['attachments'].append(attachment)
    path.write_text(json.dumps(data), encoding='utf-8')
    result = web_summary(path)
    if width < 0:
        assert result['status'] == 'BLOCKED'
        assert 'runtime viewport' in result['tests'][0]['actual']
    else:
        assert result['status'] == 'PASS'
        assert result['tests'][0]['viewport'] == '360x800'
        assert result['tests'][0]['environment']['locale'] == 'uk'


def test_deliberate_assertion_stays_failed_even_if_report_writes_successfully(tmp_path, capsys):
    path, _ = web_report(tmp_path, status='failed', overall='unexpected',
                         errors=[{'message': 'Expected Home; received Objects <script>bad</script>'}])
    output = tmp_path / 'summary'
    assert reporter.main(['--web', str(path), '--only', 'web', '--output', str(output), '--web-exit-code', '1']) == 1
    assert json.loads((output / 'summary.json').read_text())['status'] == 'FAIL'
    html = (output / 'index.html').read_text()
    assert '&lt;script&gt;bad&lt;/script&gt;' in html
    assert '<script>bad</script>' not in html
    assert len(capsys.readouterr().out) < 1000


def test_missing_browser_is_blocked_with_actionable_error(tmp_path):
    path, _ = web_report(tmp_path, status='failed', overall='unexpected',
                         errors=[{'message': "browserType.launch: Executable doesn't exist. Run playwright install."}])
    result = web_summary(path)
    assert result['status'] == 'BLOCKED'
    assert 'playwright install' in result['tests'][0]['actual']


def test_startup_failure_without_any_tests_keeps_actual_reason(tmp_path):
    path, data = web_report(tmp_path)
    data['suites'] = []
    data['stats']['expected'] = 0
    data['errors'] = [{'message': 'WebServer port 4179 is already in use'}]
    path.write_text(json.dumps(data))
    result = web_summary(path)
    assert result['status'] == 'BLOCKED'
    assert '4179' in result['tests'][0]['actual']


@pytest.mark.parametrize('value', ['{}', '[]', '{', '{"errors":null}'])
def test_malformed_or_incomplete_json_is_blocked(tmp_path, value):
    path = tmp_path / 'bad.json'
    path.write_text(value)
    assert web_summary(path)['status'] == 'BLOCKED'


def test_missing_report_does_not_reuse_previous_success(tmp_path):
    path, _ = web_report(tmp_path)
    assert web_summary(path)['status'] == 'PASS'
    path.unlink()
    assert web_summary(path)['status'] == 'BLOCKED'


def test_nonzero_process_cannot_be_hidden_by_pass_json(tmp_path):
    path, _ = web_report(tmp_path)
    result = web_summary(path, exit_codes={'web': 12})
    assert result['status'] == 'BLOCKED'
    assert '12' in result['tests'][-1]['actual']


@pytest.mark.parametrize(('status', 'expected', 'overall', 'outcome'), [
    ('skipped', 'skipped', 'skipped', 'SKIPPED'),
    ('failed', 'failed', 'expected', 'SKIPPED'),
    ('passed', 'passed', 'flaky', 'FLAKY/UNSTABLE'),
])
def test_skips_expected_failures_and_flakiness_are_not_pass(tmp_path, status, expected, overall, outcome):
    path, _ = web_report(tmp_path, status=status, expected=expected, overall=overall)
    assert web_summary(path)['status'] == outcome


def test_old_playwright_run_is_blocked_despite_fresh_file(tmp_path):
    path, _ = web_report(tmp_path)
    result = web_summary(path, identity={'started_at': '2026-01-01T00:00:02+00:00'})
    assert result['status'] == 'BLOCKED'
    assert 'earlier run' in result['tests'][0]['actual']


def test_empty_selection_and_count_mismatch_are_blocked(tmp_path):
    path, data = web_report(tmp_path)
    data['stats']['expected'] = 5
    path.write_text(json.dumps(data))
    assert web_summary(path)['status'] == 'BLOCKED'
    data['suites'] = []
    path.write_text(json.dumps(data))
    assert web_summary(path)['status'] == 'BLOCKED'


def test_native_assertion_and_capture_results_both_required(tmp_path):
    native = tmp_path / 'real-lvgl0'
    native.mkdir()
    (native / 'run.json').write_text(json.dumps({'status': 'PASS', 'frames': 774,
        'comparisons': {'matched': 468, 'different': 0, 'missing': 0}}))
    junit = tmp_path / 'junit.xml'
    junit.write_text('<testsuites><testsuite><testcase classname="native" name="bounds"/></testsuite></testsuites>')
    kwargs = dict(web=None, native_junit=junit, native_root=tmp_path, only='native')
    assert reporter.summarize(**kwargs)['status'] == 'PASS'
    junit.write_text('<testsuites><testsuite><testcase classname="native" name="bounds">'
                     '<failure>actual bottom=245, expected &lt;240</failure></testcase></testsuite></testsuites>')
    failed = reporter.summarize(**kwargs)
    assert failed['status'] == 'FAIL'
    assert 'bottom=245' in failed['tests'][0]['actual']
    (native / 'run.json').unlink()
    missing = reporter.summarize(**kwargs)
    assert missing['status'] == 'FAIL'
    assert missing['counts'] == {'FAIL': 1, 'BLOCKED': 1}


def test_source_drift_and_invalid_identity_are_reported(tmp_path, monkeypatch):
    path, _ = web_report(tmp_path)
    identity = tmp_path / 'identity.json'
    original = {'head': 'old', 'source_sha256': 'before', 'files': 1, 'started_at': '2026-01-01T00:00:00+00:00'}
    identity.write_text(json.dumps(original))
    monkeypatch.setattr(reporter, 'source_identity', lambda root: {'head': 'old', 'source_sha256': 'after', 'files': 1})
    args = ['--web', str(path), '--only', 'web', '--identity', str(identity), '--output', str(tmp_path / 'out')]
    assert reporter.main(args) == 1
    result = json.loads((tmp_path / 'out/summary.json').read_text())
    assert result['status'] == 'BLOCKED'
    assert result['tests'][-1]['id'] == 'run/source-identity'
    identity.write_text('{}')
    assert reporter.main(args) == 1


def test_init_requires_fresh_output_and_never_relabels_old_run(tmp_path, monkeypatch):
    monkeypatch.setattr(reporter, 'source_identity', lambda root: {'head': 'head', 'source_sha256': 'hash', 'files': 1})
    output = tmp_path / 'run'
    identity = reporter.initialize(output)
    assert json.loads((output / 'identity.json').read_text()) == identity
    with pytest.raises(FileExistsError):
        reporter.initialize(output)


def test_selection_manifest_catches_removed_test(tmp_path):
    path, data = web_report(tmp_path)
    data['config']['metadata']['expectedTests'] = 4
    path.write_text(json.dumps(data))
    result = web_summary(path)
    assert result['status'] == 'BLOCKED'
    assert result['tests'][-1]['actual'] == 'Expected 4 selected tests, found 1'


def test_inconsistent_web_status_cannot_pass(tmp_path):
    path, _ = web_report(tmp_path, overall='unexpected')
    assert web_summary(path)['status'] == 'FAIL'


def test_inconsistent_native_pass_counts_are_blocked(tmp_path):
    native = tmp_path / 'real-lvgl0'
    native.mkdir()
    junit = tmp_path / 'native.xml'
    junit.write_text('<testsuites><testsuite><testcase name="pixels"/></testsuite></testsuites>')
    (native / 'run.json').write_text(json.dumps({'status': 'PASS', 'frames': 774,
        'comparisons': {'matched': 467, 'different': 0, 'missing': 1}}))
    result = reporter.summarize(web=None, native_junit=junit, native_root=tmp_path, only='native')
    assert result['status'] == 'BLOCKED'
    assert 'contradicts' in result['tests'][0]['actual']


def test_component_and_domain_junit_join_selected_browser_evidence(tmp_path):
    web, _ = web_report(tmp_path)
    components = tmp_path / 'components.xml'
    components.write_text('<testsuites tests="1" failures="0"><testsuite>'
        '<testcase classname="tests/components/workflow.dom.test.tsx" name="pending"/>'
        '</testsuite></testsuites>')
    domain = tmp_path / 'domain.xml'
    domain.write_text('<testsuite tests="1"><testcase classname="resultSegment.test.ts" name="window"/></testsuite>')
    result = reporter.summarize(web=web, native_junit=None, native_root=None, only='web',
                                components_junit=components, domain_junit=domain)
    assert result['status'] == 'PASS'
    assert result['selected'] == ['web', 'components', 'domain']
    assert result['counts'] == {'PASS': 3}
    row = result['tests'][1]
    assert row['viewport'] is None
    assert row['environment'] == {'runtime': 'jsdom'}
    assert row['source']['file'] == 'tests/components/workflow.dom.test.tsx'


def test_component_assertion_failure_is_not_hidden_by_passing_web(tmp_path):
    web, _ = web_report(tmp_path)
    components = tmp_path / 'components.xml'
    components.write_text('<testsuites tests="1" failures="1"><testsuite>'
        '<testcase classname="workflow.dom.test.tsx" name="pending"><failure>expected 1 command; got 2</failure>'
        '</testcase></testsuite></testsuites>')
    result = reporter.summarize(web=web, native_junit=None, native_root=None, only='web', components_junit=components)
    assert result['status'] == 'FAIL'
    assert result['counts'] == {'PASS': 1, 'FAIL': 1}
    assert 'got 2' in result['tests'][-1]['actual']


@pytest.mark.parametrize('xml', ['<', '<testsuites/>', '<testsuite tests="2"><testcase name="only one"/></testsuite>',
    '<testsuite tests="1" failures="1"><testcase name="contradiction"/></testsuite>'])
def test_invalid_component_junit_cannot_pass(tmp_path, xml):
    path = tmp_path / 'components.xml'
    path.write_text(xml)
    result = reporter.summarize(web=None, native_junit=None, native_root=None, only='components', components_junit=path)
    assert result['status'] == 'BLOCKED'


def test_missing_domain_prerequisite_remains_blocked(tmp_path):
    result = reporter.summarize(web=None, native_junit=None, native_root=None, only='domain',
                                domain_junit=tmp_path / 'missing.xml')
    assert result['status'] == 'BLOCKED'


def test_compact_summary_prioritizes_failures_over_unselected_skips(tmp_path, capsys):
    rows = [reporter.record('components', f'skip-{n}', 'SKIPPED', '') for n in range(5)]
    rows.append(reporter.record('components', 'duplicate-command', 'FAIL', 'expected 1 command; got 2'))
    result = {'status': 'FAIL', 'counts': {'SKIPPED': 5, 'FAIL': 1}, 'selected': ['components'],
              'identity': None, 'generated_at': 'now', 'tests': rows, 'not_run': []}
    reporter.write_report(tmp_path / 'report', result)
    assert 'duplicate-command: FAIL expected 1 command; got 2' in capsys.readouterr().out
