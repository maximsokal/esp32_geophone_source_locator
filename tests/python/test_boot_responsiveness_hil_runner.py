"""Replay contracts for the boot/responsiveness HIL runner.

Saved logs can prove parser/threshold behavior, but never physical keypad
activity, current-board timing, or passive COM6 wiring. Therefore a valid
replay must report telemetry PASS while the physical HIL result stays BLOCKED.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_boot_responsiveness_hil.ps1"


def powershell_command() -> str:
    executable = shutil.which("powershell.exe") or shutil.which("pwsh")
    if executable is None:
        raise RuntimeError("PowerShell is required for boot responsiveness runner tests")
    return executable


def make_log(
    *,
    interactive_ms: int = 2800,
    startup_total_ms: int = 3200,
    recovery_ms: tuple[int, ...] = (140, 145),
    dispatch_ages: tuple[int, ...] = (15, 20, 25, 18, 22),
    dispatch_keys: tuple[int, ...] = (1, 2, 3, 4, 5),
    stale_age: int | None = None,
    safe_shutdown: bool = False,
    panic: bool = False,
    priority_current: int = 23,
    priority_base: int = 1,
    priority_inherited: int = 1,
    include_priority: bool = True,
    include_gate: bool = True,
    include_interactive: bool = True,
    cache_before_recovery_stop: bool = False,
) -> str:
    if len(dispatch_keys) != len(dispatch_ages):
        raise ValueError("dispatch_keys and dispatch_ages must have the same length")

    lines: list[str] = []
    if include_gate:
        lines.append("I APP_EVENTS: KEYPAD_EVENT_GATE state=ARMED")
    if include_interactive:
        lines.append(f"I APP: BOOT_TIMING: interactive_ready={interactive_ms} ms")
    lines.extend(
        [
            f"I APP: BOOT_TIMING: startup_total={startup_total_ms} ms",
            "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=START core=0 priority=1",
        ]
    )

    total = len(recovery_ms)
    for index, elapsed in enumerate(recovery_ms, 1):
        lines.append(
            "I OBJECT_RECOVERY: OBJECT_RECOVERY_PROGRESS "
            f"object=OBJ_{index:04d} index={index} total={total} removed=0 elapsed_ms={elapsed}"
        )

    for key, age in zip(dispatch_keys, dispatch_ages, strict=True):
        lines.append(f"I UI_EVENT: KEYPAD_EVENT_DISPATCH key={key} age_ms={age}")

    if stale_age is not None:
        lines.append(
            f"W UI_EVENT: Dropped stale keypad navigation age_ms={stale_age} total=1"
        )

    if include_priority:
        lines.append(
            "I RUNTIME_OBSERVABILITY: RUNTIME_TASK rank=1 name=web_status_cach core=0 "
            f"priority_current={priority_current} priority_base={priority_base} "
            f"inherited={priority_inherited} runtime_delta_us=1000 cpu=10.0 stack_free=1000"
        )

    if cache_before_recovery_stop:
        lines.append(
            "I WEB_STATUS_CACHE: WEB_STATUS_CACHE refresh=done objects=2 session_errors=0 elapsed_ms=500 mode=cooperative"
        )

    lines.append("I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=STOP result=0")

    if not cache_before_recovery_stop:
        lines.append(
            "I WEB_STATUS_CACHE: WEB_STATUS_CACHE refresh=done objects=2 session_errors=0 elapsed_ms=500 mode=cooperative"
        )

    if safe_shutdown:
        lines.append("W APP: Safe shutdown started")
    if panic:
        lines.append("Guru Meditation Error: Core 0 panic'ed")

    return "\n".join(lines) + "\n"


def run_replay(path: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    executable = powershell_command()
    args = [executable, "-NoLogo", "-NoProfile"]
    if executable.lower().endswith("powershell.exe"):
        args.extend(["-ExecutionPolicy", "Bypass"])
    args.extend(["-File", str(RUNNER), "-LogPath", str(path), *extra])
    return subprocess.run(
        args,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def test_valid_replay_passes_telemetry_but_never_claims_physical_hil(tmp_path: Path):
    path = tmp_path / "boot-pass.log"
    path.write_text(make_log(), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "Interactive startup latency" in result.stdout
    assert "interactive_ready=2800ms limit=5000ms startup_total=3200ms" in result.stdout
    assert "samples=2 max=145ms limit=250ms" in result.stdout
    assert "All five keypad keys reach LVGL" in result.stdout
    assert "dispatches=5/5 keys_seen=1,2,3,4,5 missing=none" in result.stdout
    assert "stale_drops=0" in result.stdout
    assert "safe_shutdown_markers=0" in result.stdout
    assert "base_priority=1 inherited_observed=True" in result.stdout
    assert "Gate -> interactive -> startup -> recovery -> Web cache ordering" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: PASS" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: BLOCKED" in result.stdout
    assert "REPLAY_LIMITATION:" in result.stdout


def test_replay_with_repeated_single_key_is_blocked_not_false_pass(tmp_path: Path):
    path = tmp_path / "boot-one-key.log"
    path.write_text(make_log(dispatch_keys=(1, 1, 1, 1, 1)), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "dispatches=5/5 keys_seen=1 missing=2,3,4,5" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: BLOCKED" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_rejects_slow_interactive_ready_and_recovery_window(tmp_path: Path):
    path = tmp_path / "boot-slow.log"
    path.write_text(
        make_log(interactive_ms=5001, startup_total_ms=6500, recovery_ms=(200, 251)),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "interactive_ready=5001ms limit=5000ms startup_total=6500ms" in result.stdout
    assert "max=251ms limit=250ms" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: FAIL" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: FAIL" in result.stdout


def test_replay_allows_slow_full_startup_after_fast_interactive_ready(tmp_path: Path):
    path = tmp_path / "boot-slow-total.log"
    path.write_text(
        make_log(interactive_ms=2800, startup_total_ms=8000),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "interactive_ready=2800ms limit=5000ms startup_total=8000ms" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: PASS" in result.stdout


def test_replay_without_interactive_ready_is_blocked(tmp_path: Path):
    path = tmp_path / "boot-old-log.log"
    path.write_text(make_log(include_interactive=False), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "BOOT_TIMING interactive_ready" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: BLOCKED" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_without_gate_armed_is_blocked(tmp_path: Path):
    path = tmp_path / "boot-no-gate.log"
    path.write_text(make_log(include_gate=False), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "KEYPAD_EVENT_GATE state=ARMED" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: BLOCKED" in result.stdout


def test_replay_rejects_stale_keypad_and_spontaneous_shutdown(tmp_path: Path):
    path = tmp_path / "boot-input-fail.log"
    path.write_text(make_log(stale_age=301, safe_shutdown=True), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "stale_drops=1" in result.stdout
    assert "safe_shutdown_markers=1" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: FAIL" in result.stdout


def test_replay_rejects_cache_refresh_before_recovery_stop(tmp_path: Path):
    path = tmp_path / "boot-order-fail.log"
    path.write_text(make_log(cache_before_recovery_stop=True), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "Web status cache completed before background recovery stopped" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: FAIL" in result.stdout


def test_replay_rejects_observed_web_cache_base_priority_regression(tmp_path: Path):
    path = tmp_path / "boot-priority-fail.log"
    path.write_text(
        make_log(priority_current=23, priority_base=2, priority_inherited=1),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "priority_base=2 expected=1" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: FAIL" in result.stdout


def test_replay_allows_missing_priority_sample_without_inventing_failure(tmp_path: Path):
    path = tmp_path / "boot-no-priority.log"
    path.write_text(make_log(include_priority=False), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "Web cache priority inheritance diagnostic" in result.stdout
    assert "SKIPPED" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: PASS" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_missing_core_markers_is_blocked_not_pass(tmp_path: Path):
    path = tmp_path / "boot-missing.log"
    path.write_text("I APP: unrelated log line\n", encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: BLOCKED" in result.stdout
    assert "BOOT_RESPONSIVENESS_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_rejects_target_panic(tmp_path: Path):
    path = tmp_path / "boot-panic.log"
    path.write_text(make_log(panic=True), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "failure_markers=1" in result.stdout
    assert "BOOT_RESPONSIVENESS_TELEMETRY_RESULT: FAIL" in result.stdout


def test_live_runner_keeps_programming_and_passive_log_ports_separate_by_contract():
    source = RUNNER.read_text(encoding="utf-8")

    assert '[string]$FlashPort = "COM4"' in source
    assert '[string]$LogPort = "COM6"' in source
    assert '"-Action", "monitor"' in source
    assert '-Action flash' in source
    assert 'if ($FlashPort -eq $LogPort)' in source
    assert "FlashPort and LogPort must be different" in source
    assert "press LEFT, UP, DOWN, RIGHT and OK" in source
    assert "background recovery and the following Web status-cache refresh" in source
    assert "KEYPAD_EVENT_DISPATCH" not in source  # parsing lives in the sourced contract
    assert "BOOT_RESPONSIVENESS_HIL_RESULT" in source
