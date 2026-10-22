"""Real response builders and capture/report trust boundaries, no board needed."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import render_web_pages as producer


@pytest.fixture(scope='module')
def captured(tmp_path_factory):
    output = tmp_path_factory.mktemp('firmware-pages')
    run = producer.capture(output, ROOT / '.test-build/ui-firmware-pages/native')
    return output, run, {row['id']: row for row in run['cases']}


def test_inventory_identity_and_exact_assets(captured):
    output, run, cases = captured
    assert run['status'] == 'PASS' and set(cases) == producer.CASES
    assert run['inputs'] == producer.inputs()
    assert run['binary_sha256']
    for row in cases.values():
        assert row['sha256'] == producer.file_hash(output / row['file'])
    for name, digest in run['assets'].items():
        assert producer.file_hash(output / 'assets' / name) == digest
        assert (output / 'assets' / name).read_bytes() == (ROOT / 'components/web_server/assets' / name).read_bytes()


@pytest.mark.parametrize('name', ['wifi-form', 'wifi-saved', 'ca-guide', 'verify-ui', 'fallback', 'seismic',
                                 'ca-legacy', 'ca-confirmed', 'verify-legacy'])
def test_real_html_headers_and_markup(captured, name):
    output, _, cases = captured
    row = cases[name]
    assert row['status'] == 200 and row['handler_result'] == 0
    assert row['content_type'] == 'text/html; charset=utf-8' and row['cache_control'] == 'no-store'
    body = (output / row['file']).read_text(encoding='utf-8')
    assert body.lower().startswith('<!doctype html>') and 'name=viewport' in body.replace('name="viewport"', 'name=viewport')
    if name == 'fallback':
        original = (ROOT / 'components/web_server/assets/index.html').read_text(encoding='utf-8')
        assert body == original.replace('{{WIFI_SSID}}', 'GEOLOCATOR_SETUP')
    if name == 'seismic':
        assert (output / row['file']).read_bytes() == (ROOT / 'components/web_server/assets/seismic.html').read_bytes()


def test_actual_redirects_errors_and_credentials_admission(captured):
    output, _, cases = captured
    assert cases['wifi-needs-ca']['location'] == 'http://192.168.4.1/setup/ca-guide'
    assert cases['wifi-station-redirect']['location'] == 'https://geolocator.local/'
    expected = {'wifi-shutdown': (409, 'Shutdown is in progress'), 'wifi-not-setup': (403, 'only available in setup mode'),
                'wifi-ca-required': (403, 'Confirm GeoLocator Root CA'), 'wifi-invalid-body': (400, 'Invalid form body'),
                'wifi-invalid-form': (400, 'Invalid Wi-Fi settings'), 'wifi-save-error': (500, 'Failed to save Wi-Fi settings'),
                'ca-unavailable': (500, 'Root CA is not embedded')}
    for name, (status, text) in expected.items():
        assert cases[name]['status'] == status
        assert text in (output / cases[name]['file']).read_text(encoding='utf-8')


def test_actual_verification_reports_cover_every_capability_mask(captured):
    output, _, cases = captured
    for mask in range(16):
        response = json.loads((output / cases[f'verify-report-{mask}']['file']).read_text())
        assert response == {'ready': mask == 15, 'mask': mask}
    assert cases['verify-expired']['status'] == 403
    assert cases['verify-report-expired']['status'] == 403
    assert cases['verify-report-invalid']['status'] == 400
    assert cases['ca-legacy-unavailable']['status'] == 503


def test_failed_capture_replaces_old_pass(tmp_path, monkeypatch):
    (tmp_path / 'run.json').write_text('{"status":"PASS"}')
    monkeypatch.setattr(producer, 'compiler_from_args', lambda _: None)
    with pytest.raises(RuntimeError, match='Native C compiler'):
        producer.capture(tmp_path, tmp_path / 'build')
    assert json.loads((tmp_path / 'run.json').read_text())['status'] == 'FAIL'


def test_producer_refuses_source_output():
    for path in (ROOT, ROOT / 'tests/ui_render/golden', ROOT / 'components/web_server/assets'):
        with pytest.raises(ValueError, match='overwrite repository sources'):
            producer.capture(path, ROOT / '.test-build/unused')


@pytest.mark.parametrize('fault', ['missing', 'failed', 'source', 'response', 'asset'])
def test_browser_loader_rejects_missing_failed_stale_or_corrupt_capture(captured, tmp_path, fault):
    output, _, _ = captured
    target = tmp_path / 'capture'
    shutil.copytree(output, target)
    record = target / 'run.json'
    run = json.loads(record.read_text(encoding='utf-8'))
    if fault == 'missing':
        record.unlink()
    elif fault in ('failed', 'source'):
        if fault == 'failed':
            run['status'] = 'FAIL'
        else:
            run['inputs'][next(iter(run['inputs']))] = '0' * 64
        record.write_text(json.dumps(run), encoding='utf-8')
    else:
        path = target / ('wifi-form.html' if fault == 'response' else 'assets/seismic.js')
        path.write_bytes(path.read_bytes() + b'corrupt')
    loader = (ROOT / 'webui/tests/e2e/fixtures/firmware-pages-producer.mjs').as_uri()
    node = shutil.which('node')
    assert node, 'Node is required to validate the actual browser producer loader'
    result = subprocess.run([node, '--input-type=module', '-e',
        f'import {{loadFirmwarePages}} from {json.dumps(loader)}; await loadFirmwarePages();'],
        cwd=ROOT, env={**os.environ, 'FIRMWARE_PAGES_DIR': str(target)}, capture_output=True, text=True)
    assert result.returncode != 0
    assert {'missing': 'ENOENT', 'failed': 'Native producer must finish successfully',
            'source': 'Stale native producer input', 'response': 'Captured response hash',
            'asset': 'Captured asset hash'}[fault] in result.stderr
