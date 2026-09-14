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
    (root / "nRF54L15" / "pio.ps1").write_text("# nRF runner\n")
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
        str(root / "build" / "host-tests-a-first"),
        "--keep-going",
    )


def test_software_plan_is_local_and_never_contains_firmware_or_github(tmp_path: Path):
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
    assert "web/npm-ci" in names
    assert "web/typecheck" in names
    assert "web/console" in names
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
        "web/console",
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


def test_nrf_plan_uses_host_policy_then_path_safe_wrapper(tmp_path: Path):
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
    ]
    assert steps[0].command[0] == "python-test"
    assert "coverage-nrf.json" in steps[0].command[steps[0].command.index("--manifest") + 1]
    assert steps[1].command[0] == "powershell-test"
    assert steps[2].command[0] == "powershell-test"
    assert any("pio.ps1" in argument for argument in steps[1].command)
    assert any("pio.ps1" in argument for argument in steps[2].command)
    assert steps[1].command[-2:] == ("-Action", "build")
    assert steps[2].command[-2:] == ("-Action", "test")
    assert all("pio run -d" not in " ".join(step.command) for step in steps)


def test_all_scope_does_not_duplicate_nrf_host_policy(tmp_path: Path):
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
