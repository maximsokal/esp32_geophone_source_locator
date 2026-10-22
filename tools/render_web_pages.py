"""Capture production firmware HTML with native SDK/HTTP fakes, without a device."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess

from run_host_tests import compiler_from_args, compiler_description, ensure_native_compiler, is_msvc, load_msvc_environment
from run_ui_render_tests import file_hash, find_tool, write_record

ROOT = Path(__file__).resolve().parents[1]
CASES = {'wifi-form', 'wifi-needs-ca', 'wifi-ca-required', 'wifi-station-redirect',
         'wifi-not-setup', 'wifi-shutdown', 'wifi-invalid-body', 'wifi-invalid-form',
         'wifi-save-error', 'wifi-saved', 'ca-guide', 'ca-unavailable', 'verify-ui', 'fallback', 'seismic',
         'ca-legacy', 'ca-confirmed', 'ca-legacy-unavailable', 'verify-expired', 'verify-legacy', 'verify-report-invalid', 'verify-report-expired',
         *(f'verify-report-{mask}' for mask in range(16))}


def inputs() -> dict[str, str]:
    paths = [ROOT / 'tools/render_web_pages.py', ROOT / 'tools/run_host_tests.py',
             ROOT / 'tools/run_ui_render_tests.py', ROOT / 'tests/ui_render/web_pages_harness.c',
             ROOT / 'components/app_config/include/app_config.h', ROOT / 'components/app_events/include/app_events.h']
    for directory in ('components/web_server', 'tests/ui_render/web_pages', 'tests/host/stubs/seismic_pipeline',
                      'tests/host/stubs/object_store_profile'):
        paths.extend(p for p in (ROOT / directory).rglob('*') if p.is_file() and
                     (p.suffix in {'.c', '.h', '.css', '.html', '.js'} or p.name == 'CMakeLists.txt'))
    return {p.relative_to(ROOT).as_posix(): file_hash(p) for p in sorted(set(paths))}


def capture(output: Path, build_dir: Path) -> dict:
    output = output.resolve()
    # Only disposable workspace outputs or external temporary directories are writable.
    if output == ROOT or output in ROOT.parents or (output.is_relative_to(ROOT) and
            not output.is_relative_to(ROOT / '.test-build')):
        raise ValueError('Capture output must not overwrite repository sources')
    output.mkdir(parents=True, exist_ok=True)
    record = {'status': 'RUNNING', 'started_at': datetime.now(timezone.utc).isoformat(),
              'inputs': inputs(), 'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'environment': {'platform': platform.platform(), 'python': platform.python_version()},
              'evidence': 'Production C response builders; SDK/radio/events simulated; CA not embedded'}
    write_record(output / 'run.json', record)
    try:
        compiler = compiler_from_args(None)
        if not compiler:
            raise RuntimeError('Native C compiler is required for firmware page capture')
        description = compiler_description(compiler)
        ensure_native_compiler(compiler, description)
        env = load_msvc_environment(compiler) if is_msvc(compiler) else os.environ.copy()
        env['VSLANG'] = '1033'
        build_dir = build_dir.resolve()
        build_dir.mkdir(parents=True, exist_ok=True)
        commands = [
            [find_tool('cmake'), '-S', str(ROOT / 'tests/ui_render/web_pages'), '-B', str(build_dir),
             '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={find_tool("ninja")}',
             f'-DCMAKE_C_COMPILER={compiler[0]}', '-DCMAKE_BUILD_TYPE=Release'],
            [find_tool('cmake'), '--build', str(build_dir), '--clean-first', '--parallel', '4'],
        ]
        executable = build_dir / ('web_pages.exe' if os.name == 'nt' else 'web_pages')
        commands.append([str(executable), str(ROOT / 'components/web_server/assets'), str(output)])
        for name, command in zip(('configure', 'build', 'capture'), commands):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=120)
            log = output / f'{name}.log'
            log.write_text(result.stdout + result.stderr, encoding='utf-8')
            if result.returncode:
                raise RuntimeError(f'{name} failed ({result.returncode}); see {log}\n{(result.stdout + result.stderr)[-5000:]}')
        responses = json.loads((output / 'responses.json').read_text(encoding='utf-8'))['cases']
        if len(responses) != len(CASES) or {row['id'] for row in responses} != CASES:
            raise ValueError('Incomplete production response inventory')
        for response in responses:
            path = output / response['file']
            if path.parent != output or path.stat().st_size != response['bytes']:
                raise ValueError('Invalid response path/size')
            response['sha256'] = file_hash(path)
        assets = output / 'assets'
        assets.mkdir(exist_ok=True)
        for name in ('dashboard.css', 'dashboard.js', 'seismic.css', 'seismic.js'):
            shutil.copyfile(ROOT / 'components/web_server/assets' / name, assets / name)
        if record['inputs'] != inputs():
            raise ValueError('Producer source changed during capture')
        record.update(status='PASS', compiler={'command': compiler, 'description': description,
                      'sha256': file_hash(Path(shutil.which(compiler[0]) or compiler[0]))},
                      binary_sha256=file_hash(executable), cases=responses,
                      assets={p.name: file_hash(p) for p in sorted(assets.iterdir())},
                      finished_at=datetime.now(timezone.utc).isoformat())
    except Exception as error:
        record.update(status='FAIL', error=str(error), finished_at=datetime.now(timezone.utc).isoformat())
        write_record(output / 'run.json', record)
        raise
    write_record(output / 'run.json', record)
    print(f'FIRMWARE_PAGES_PRODUCER: PASS ({len(responses)} responses; {output})')
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.test-build/ui-firmware-pages/producer')
    parser.add_argument('--build-dir', type=Path, default=ROOT / '.test-build/ui-firmware-pages/native')
    args = parser.parse_args()
    capture(args.output, args.build_dir)


if __name__ == '__main__':
    main()
