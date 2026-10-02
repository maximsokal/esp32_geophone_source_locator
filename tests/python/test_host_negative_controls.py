"""Negative controls for host tests that must fail by design (MAX-372).

The control manifest lives outside tests/host/manifest.d so CI and the final
gate, which require every fragment to pass, never run it directly.
"""
from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_host_tests.py"
CONTROL_MANIFEST = REPO_ROOT / "tests" / "host" / "negative_controls" / "seismic_buffer_broken_publish.json"
CONTROL_NAME = "seismic_buffer_broken_publish_control"


def _native_compiler_available() -> bool:
    spec = importlib.util.spec_from_file_location("run_host_tests_for_controls", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.compiler_from_args(None) is not None


@pytest.fixture(scope="module", autouse=True)
def _require_compiler() -> None:
    if not _native_compiler_available():
        pytest.skip("no native C compiler found by tools/run_host_tests.py (HOST_CC/CC/PATH/MSVC)")


def _run(args: list[str], build_dir: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(RUNNER), *args, "--build-dir", str(build_dir)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=300,
    )


def test_normal_seismic_buffer_target_passes(tmp_path: Path) -> None:
    result = _run(["--only", "seismic_buffer"], tmp_path / "normal")
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "[PASS] seismic_buffer" in output, output


def test_broken_publish_control_fails_in_run_stage_on_ordering_assertion(tmp_path: Path) -> None:
    result = _run(["--manifest", str(CONTROL_MANIFEST)], tmp_path / "control")
    output = result.stdout + result.stderr
    assert result.returncode != 0, "negative control passed: the torn-read test no longer detects broken publish\n" + output
    assert f"[FAIL] {CONTROL_NAME} (run)" in output, output
    assert f"[FAIL] {CONTROL_NAME} (compile)" not in output, output
    # The fixture's own sentinel must not be what fired.
    assert "broken publish fixture unexpectedly passed" not in output, output
    # assert_frame() in the torn-read probe must be the failing assertion.
    assert "assert" in output.lower() and "frame->" in output and "seismic_buffer_test.c" in output, output
