from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO_ROOT / "tools" / "run_local_verify.py"

_spec = importlib.util.spec_from_file_location("run_local_verify", RUNNER_PATH)
assert _spec is not None and _spec.loader is not None
runner = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = runner
_spec.loader.exec_module(runner)


def make_repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "tools").mkdir(parents=True)
    (root / "tests" / "host" / "manifest.d").mkdir(parents=True)
    (root / "tests" / "python").mkdir(parents=True)
    (root / "webui").mkdir(parents=True)
    (root / "nRF54L15").mkdir(parents=True)

    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.16)\n")
    (root / "tools" / "run_host_tests.py").write_text("# host runner\n")
    (root / "tools" / "run_idf.ps1").write_text("# idf runner\n")
    (root / "nRF54L15" / "ncs.ps1").write_text("# canonical nRF runner\n")
    (root / "nRF54L15" / "pio.ps1").write_text("# compatibility nRF runner\n")
    (root / "tests" / "host" / "manifest.json").write_text(
        '{"version": 1, "tests": [{"name": "base"}]}\n'
    )
    (root / "tests" / "host" / "manifest.d" / "z-last.json").write_text("{}\n")
    (root / "tests" / "host" / "manifest.d" / "a-first.json").write_text("{}\n")
    (root / "tests" / "host" / "manifest.d" / "coverage-nrf.json").write_text(
        '{"version": 1, "tests": []}\n'
    )
    (root / "webui" / "package.json").write_text('{"name": "fake"}\n')
    return root


def test_host_plan_runs_base_then_sorted_manifest_fragments(tmp_path: Path):
    root = make_repo(tmp_path)
    steps = runner.host_steps(root, "python-test")
    assert [step.name for step in steps] == [
        "host/base",
        "host/a-first",
        "host/coverage-nrf",
        "host/z-last",
    ]
    assert steps[0].command[-1] == "--keep-going"
    assert steps[1].command[-3:] == (
        "--build-dir",
        str(root / ".test-build" / "host-tests-a-first"),
        "--keep-going",
    )


def test_software_plan_is_local_and_never_contains_firmware_or_github(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(runner.shutil, "which", lambda name: "ast-grep-test")
    root = make_repo(tmp_path)
    steps = runner.build_plan(
        root,
        scope="software",
        build_dir="build",
        jobs=2,
        skip_npm_ci=False,
        python_executable="python-test",
        npm_executable="npm-test",
        system_name="Windows",
        powershell_executable="powershell-test",
        idf_executable=None,
    )
    names = [step.name for step in steps]
    assert names[:4] == [
        "host/base",
        "host/a-first",
        "host/coverage-nrf",
        "host/z-last",
    ]
    assert "python/deterministic" in names
    assert "ast/rules" in names
    assert "ast/scan" in names
    assert "web/npm-ci" in names
    assert "web/typecheck" in names
    assert "web/test-types" in names
    assert "web/console" in names
    assert "web/domain" in names
    assert "web/components" in names
    assert "web/xr" in names
    assert "web/build" in names
    assert "web/bundle" in names
    assert not any(name.startswith("firmware/") for name in names)

    flattened = " ".join(
        argument.lower()
        for step in steps
        for argument in step.command
    )
    assert "github" not in flattened
    assert "gh " not in flattened


def test_skip_npm_ci_keeps_other_web_checks(tmp_path: Path):
    root = make_repo(tmp_path)
    steps = runner.web_steps(root, "npm-test", skip_npm_ci=True)
    names = [step.name for step in steps]
    assert "web/npm-ci" not in names
    assert names == [
        "web/typecheck",
        "web/test-types",
        "web/console",
        "web/domain",
        "web/components",
        "web/xr",
        "web/build",
        "web/bundle",
    ]


def test_windows_firmware_plan_uses_existing_local_idf_helper(tmp_path: Path):
    root = make_repo(tmp_path)
    steps = runner.firmware_steps(
        root,
        build_dir="build-local",
        jobs=4,
        system_name="Windows",
        powershell_executable=r"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        idf_executable=None,
    )
    assert len(steps) == 1
    command = steps[0].command
    assert steps[0].name == "firmware/esp-idf"
    assert str(root / "tools" / "run_idf.ps1") in command
    assert "build" in command
    assert "build-local" in command
    assert command[-2:] == ("-Jobs", "4")


def test_nrf_plan_uses_host_policy_then_canonical_ncs_wrapper(tmp_path: Path):
    root = make_repo(tmp_path)
    steps = runner.build_plan(
        root,
        scope="nrf",
        build_dir="build",
        jobs=2,
        skip_npm_ci=True,
        python_executable="python-test",
        npm_executable=None,
        system_name="Windows",
        powershell_executable="powershell-test",
        idf_executable=None,
    )

    assert [step.name for step in steps] == [
        "nrf/host-policy",
        "nrf/build",
        "nrf/tests",
        "nrf/cs-tests",
    ]
    assert steps[0].command[0] == "python-test"
    assert "coverage-nrf.json" in steps[0].command[steps[0].command.index("--manifest") + 1]
    assert steps[1].command[0] == "powershell-test"
    assert any("ncs.ps1" in argument for argument in steps[1].command)
    assert not any("pio.ps1" in argument for argument in steps[1].command)
    assert steps[1].command[-2:] == ("-Action", "build")
    assert steps[2].skip_reason is None
    assert steps[2].command[-2:] == ("-Action", "ncs-test-build")
    assert steps[3].skip_reason is None
    assert steps[3].command[-2:] == ("-Action", "ncs-cs-test-build")


def test_all_scope_does_not_duplicate_nrf_host_policy(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(runner.shutil, "which", lambda name: "ast-grep-test")
    root = make_repo(tmp_path)
    steps = runner.build_plan(
        root,
        scope="all",
        build_dir="build",
        jobs=2,
        skip_npm_ci=True,
        python_executable="python-test",
        npm_executable="npm-test",
        system_name="Windows",
        powershell_executable="powershell-test",
        idf_executable=None,
    )
    names = [step.name for step in steps]
    assert names.count("host/coverage-nrf") == 1
    assert "nrf/host-policy" not in names
    nrf_build = next(step for step in steps if step.name == "nrf/build")
    assert any("ncs.ps1" in arg for arg in nrf_build.command)


def test_ast_scope_requires_installed_cli(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(runner.shutil, "which", lambda name: None)
    with pytest.raises(runner.VerificationError, match="ast-grep verification"):
        runner.ast_steps(tmp_path)


def test_ast_scan_reports_violation_but_allows_bus_owner(tmp_path: Path):
    import shutil
    import subprocess

    executable = shutil.which("ast-grep")
    if executable is None:
        pytest.skip("ast-grep CLI is not installed")
    shutil.copy(REPO_ROOT / "sgconfig.yml", tmp_path)
    shutil.copytree(REPO_ROOT / "tools" / "ast-grep", tmp_path / "tools" / "ast-grep")
    driver = tmp_path / "components" / "ina3221_monitor" / "driver.c"
    owner = tmp_path / "components" / "sensor_bus" / "sensor_bus.c"
    driver.parent.mkdir(parents=True)
    owner.parent.mkdir(parents=True)
    bad_call = "void init(void) { i2c_new_master_bus(&config, &bus); }\n"
    owner.write_text(bad_call)
    driver.write_text("/* i2c_new_master_bus(&config, &bus); */\n")
    command = (executable, "scan", "--config", str(tmp_path / "sgconfig.yml"), "components")
    assert subprocess.run(command, cwd=tmp_path, capture_output=True).returncode == 0
    driver.write_text(bad_call)
    result = subprocess.run(command, cwd=tmp_path, capture_output=True, text=True)
    assert result.returncode != 0
    assert "ina3221-shared-bus" in result.stdout + result.stderr


def test_non_windows_firmware_does_not_fall_back_to_cloud(tmp_path: Path):
    root = make_repo(tmp_path)
    with pytest.raises(runner.VerificationError, match="GitHub Actions are intentionally not used"):
        runner.firmware_steps(
            root,
            build_dir="build",
            jobs=2,
            system_name="Linux",
            powershell_executable=None,
            idf_executable=None,
        )


def test_non_windows_nrf_does_not_fake_canonical_windows_wrapper(tmp_path: Path):
    root = make_repo(tmp_path)
    with pytest.raises(runner.VerificationError, match="Windows path-safe"):
        runner.nrf_steps(
            root,
            python_executable="python-test",
            system_name="Linux",
            powershell_executable=None,
        )


def test_invalid_jobs_are_rejected_before_build(tmp_path: Path):
    root = make_repo(tmp_path)
    with pytest.raises(runner.VerificationError, match="1..32"):
        runner.firmware_steps(
            root,
            build_dir="build",
            jobs=0,
            system_name="Windows",
            powershell_executable="powershell-test",
            idf_executable=None,
        )


def test_ui_plan_owns_fresh_reports_and_keeps_existing_default_scopes(tmp_path):
    root = make_repo(tmp_path)
    output = root / '.test-build/ui-regression/run'
    steps = runner.ui_steps(root, 'python-test', 'npm-test', skip_npm_ci=True, output=output)
    assert [s.name for s in steps] == ['ui-native/bootstrap', 'ui-native/tests',
        'ui-web/typecheck', 'ui-web/test-types', 'ui-web/console', 'ui-web/domain',
        'ui-web/components', 'ui-web/e2e-types', 'ui-web/smoke', 'ui-web/bundle']
    assert '--junitxml' in steps[1].command
    assert str(output / 'native') in steps[1].command
    smoke = steps[-2]
    assert smoke.command[-3:] == ('--', '--output', str(output / 'web/results'))
    assert smoke.environment['PLAYWRIGHT_JSON_OUTPUT_FILE'] == str(output / 'web/playwright.json')
    for name in ('components', 'domain'):
        step = next(s for s in steps if s.name == f'ui-web/{name}')
        assert step.command[-2:] == ('--outputFile', str(output / f'{name}.xml'))
        assert '--reporter=junit' in step.command
    assert not runner.scope_includes('software', 'ui')
    assert not any('ui-' in s.name for s in runner.web_steps(root, 'npm-test', skip_npm_ci=True))


def test_ui_failure_still_runs_independent_group_and_report_and_preserves_exit(tmp_path, monkeypatch):
    from types import SimpleNamespace
    root = make_repo(tmp_path)
    called = []
    reports = []
    stale_log = root / '.test-build/ui-render/native/build.log'
    stale_log.parent.mkdir(parents=True)
    stale_log.write_text('older run')
    import os
    os.utime(stale_log, (1, 1))

    def initialize(output):
        output.mkdir(parents=True, exist_ok=False)

    monkeypatch.setitem(sys.modules, 'ui_report', SimpleNamespace(
        initialize=initialize, main=lambda args: reports.append(args) or 0))

    def run(step):
        called.append(step.name)
        if step.name == 'ui-native/bootstrap':
            raise runner.VerificationError('Missing compiler/dependency')
        return 'PASS'
    monkeypatch.setattr(runner, 'run_step', run)
    assert runner.run_ui_verification(root, 'python-test', 'npm-test', skip_npm_ci=True) == 1
    assert 'ui-native/tests' not in called
    assert 'ui-web/smoke' in called
    assert len(reports) == 1
    assert reports[0][reports[0].index('--native-exit-code') + 1] == '1'
    import json
    execution_path, = (root / '.test-build/ui-regression').glob('*/execution.json')
    assert json.loads(execution_path.read_text())[1]['status'] == 'NOT RUN'
    assert not (execution_path.parent / 'native-build').exists()


def test_ui_preserves_current_build_logs_inside_each_run(tmp_path, monkeypatch):
    from types import SimpleNamespace
    root = make_repo(tmp_path)
    monkeypatch.setitem(sys.modules, 'ui_report', SimpleNamespace(
        initialize=lambda output: output.mkdir(parents=True, exist_ok=False), main=lambda args: 0))
    build_dir = root / '.test-build/ui-render/native'

    def run(step):
        if step.name == 'ui-native/tests':
            build_dir.mkdir(parents=True)
            for name in ('configure.log', 'build.log', 'build.json'):
                (build_dir / name).write_text(f'current {name}')
        return 'PASS'
    monkeypatch.setattr(runner, 'run_step', run)
    assert runner.run_ui_verification(root, 'python-test', 'npm-test', skip_npm_ci=True) == 0
    copied, = (root / '.test-build/ui-regression').glob('*/native-build')
    assert {p.name: p.read_text() for p in copied.iterdir()} == {
        name: f'current {name}' for name in ('configure.log', 'build.log', 'build.json')}
