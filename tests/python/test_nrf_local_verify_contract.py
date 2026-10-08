from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO_ROOT / "tools" / "run_local_verify.py"

_spec = importlib.util.spec_from_file_location("run_local_verify_nrf_contract", RUNNER_PATH)
assert _spec is not None and _spec.loader is not None
runner = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = runner
_spec.loader.exec_module(runner)


def make_repo(tmp_path: Path, *, with_software_suite: bool) -> Path:
    root = tmp_path / "repo"
    (root / "nRF54L15" / "test").mkdir(parents=True)
    (root / "nRF54L15" / "ncs.ps1").write_text("# canonical wrapper\n", encoding="utf-8")
    (root / "nRF54L15" / "pio.ps1").write_text("# compatibility wrapper\n", encoding="utf-8")
    (root / "tools").mkdir(parents=True)
    (root / "tools" / "run_host_tests.py").write_text("# host runner\n", encoding="utf-8")
    manifest_dir = root / "tests" / "host" / "manifest.d"
    manifest_dir.mkdir(parents=True)
    (manifest_dir / "coverage-nrf.json").write_text('{"version": 1, "tests": []}\n', encoding="utf-8")
    if with_software_suite:
        (root / "nRF54L15" / "test" / "test_sw_protocol").mkdir()
    return root


def test_nrf_scope_runs_host_policy_and_canonical_ncs_build(tmp_path: Path):
    root = make_repo(tmp_path, with_software_suite=False)

    steps = runner.nrf_steps(
        root,
        python_executable="python-test",
        system_name="Windows",
        powershell_executable="powershell-test",
    )

    assert [step.name for step in steps] == [
        "nrf/host-policy",
        "nrf/build",
        "nrf/tests",
        "nrf/cs-tests",
    ]
    assert steps[0].skip_reason is None
    assert steps[0].command[0] == "python-test"
    assert "coverage-nrf.json" in steps[0].command[steps[0].command.index("--manifest") + 1]
    assert str(root / ".test-build" / "host-tests-coverage-nrf") in steps[0].command

    build = steps[1]
    assert build.skip_reason is None
    assert "ncs.ps1" in build.command[build.command.index("-File") + 1]
    assert build.command[-2:] == ("-Action", "build")
    assert "pio.ps1" not in " ".join(build.command)

    assert steps[2].skip_reason is None
    assert steps[2].command[-2:] == ("-Action", "ncs-test-build")
    assert steps[3].name == "nrf/cs-tests"
    assert steps[3].skip_reason is None
    assert steps[3].command[-2:] == ("-Action", "ncs-cs-test-build")


def test_nrf_embedded_test_builds_are_independent_of_platformio_compatibility_suite(tmp_path: Path):
    root = make_repo(tmp_path, with_software_suite=True)

    steps = runner.nrf_steps(
        root,
        python_executable="python-test",
        system_name="Windows",
        powershell_executable="powershell-test",
    )

    assert steps[2].skip_reason is None
    assert steps[3].skip_reason is None
    assert all("pio.ps1" not in " ".join(step.command) for step in steps[2:])


def test_nrf_steps_can_omit_host_policy_when_all_scope_already_runs_host_manifests(tmp_path: Path):
    root = make_repo(tmp_path, with_software_suite=False)

    steps = runner.nrf_steps(
        root,
        python_executable="python-test",
        system_name="Windows",
        powershell_executable="powershell-test",
        include_host_policy=False,
    )

    assert [step.name for step in steps] == ["nrf/build", "nrf/tests", "nrf/cs-tests"]


def test_run_step_does_not_execute_an_explicitly_skipped_step(monkeypatch, tmp_path: Path):
    called = False

    def forbidden_run(*args, **kwargs):
        nonlocal called
        called = True
        raise AssertionError("subprocess.run must not be called for skipped steps")

    monkeypatch.setattr(runner.subprocess, "run", forbidden_run)
    step = runner.Step(
        "diagnostic/optional",
        tmp_path,
        ("<optional-runner>",),
        skip_reason="hardware-only step",
    )

    assert runner.run_step(step) == "SKIPPED"
    assert called is False
