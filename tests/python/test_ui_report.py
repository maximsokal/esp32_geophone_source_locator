import importlib.util
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


def test_pass_has_actual_source_selection_and_evidence(tmp_path):
    path, _ = web_report(tmp_path)
    result = web_summary(path)
    assert result['status'] == 'PASS'
    assert result['selected'] == ['web']
    assert result['tests'][0]['source'] == {'file': 'navigation.spec.ts', 'line': 8}
    assert result['tests'][0]['environment']['evidence'] == 'isolated reporter fixture'
    assert result['tests'][0]['root_cause'] is None
    assert 'Physical' in result['not_run'][0]


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
