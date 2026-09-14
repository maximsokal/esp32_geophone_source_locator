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
    (root / "nRF54L15" / "pio.ps1").write_text("# wrapper\n", encoding="utf-8")
    if with_software_suite:
        (root / "nRF54L15" / "test" / "test_sw_protocol").mkdir()
    return root


def test_nrf_test_step_is_explicitly_skipped_when_no_software_suite_exists(tmp_path: Path):
    root = make_repo(tmp_path, with_software_suite=False)

    steps = runner.nrf_steps(
        root,
        system_name="Windows",
        powershell_executable="powershell-test",
    )

    assert [step.name for step in steps] == ["nrf/build", "nrf/tests"]
    assert steps[0].skip_reason is None
    assert steps[1].skip_reason is not None
    assert "test_sw_*" in steps[1].skip_reason


def test_nrf_test_step_runs_only_after_a_qualified_software_suite_exists(tmp_path: Path):
    root = make_repo(tmp_path, with_software_suite=True)

    steps = runner.nrf_steps(
        root,
        system_name="Windows",
        powershell_executable="powershell-test",
    )

    assert steps[1].skip_reason is None
    assert steps[1].command[-2:] == ("-Action", "test")


def test_run_step_does_not_execute_a_skipped_nrf_test(monkeypatch, tmp_path: Path):
    called = False

    def forbidden_run(*args, **kwargs):
        nonlocal called
        called = True
        raise AssertionError("subprocess.run must not be called for skipped steps")

    monkeypatch.setattr(runner.subprocess, "run", forbidden_run)
    step = runner.Step(
        "nrf/tests",
        tmp_path,
        ("powershell-test", "-File", "pio.ps1", "-Action", "test"),
        skip_reason="no qualified test_sw_* suite",
    )

    assert runner.run_step(step) == "SKIPPED"
    assert called is False
