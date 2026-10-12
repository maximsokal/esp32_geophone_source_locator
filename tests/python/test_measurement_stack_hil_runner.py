"""Contract tests for the non-destructive measurement-service stack HIL runner."""

import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_measurement_stack_hil.ps1"


def make_log(*, free: int = 4096, cycles: int = 20, failure: str = "") -> str:
    lines = [
        "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)",
        f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=idle free={free} margin=1024",
    ]
    for cycle in range(cycles):
        lines.extend(
            [
                f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=after-object free={free} margin=1024",
                f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=before-start free={free} margin=1024",
                f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=after-recorder-start free={free} margin=1024",
                f"I MEASUREMENT_SERVICE: Recording started state=RECORDING object=OBJ_9000 session=S{cycle + 1:06d}",
                f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=after-stop free={free} margin=1024",
                f"I MEASUREMENT_SERVICE: Recording stopped state=REVIEW session=S{cycle + 1:06d} samples=10 drops=0",
                f"I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=idle free={free} margin=1024",
            ]
        )
    if failure:
        lines.append(failure)
    return "\n".join(lines) + "\n"


def run_replay(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(RUNNER),
            "-LogPath",
            str(path),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def test_replay_validates_hwm_and_stays_non_destructive(tmp_path: Path):
    path = tmp_path / "measurement-stack.log"
    path.write_text(make_log(), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "STACK_HWM safety margin" in result.stdout
    assert "PASS      minimum measurement_service free=4096" in result.stdout
    assert "Measurement state REVIEW" in result.stdout
    assert "Live 20-cycle execution" in result.stdout
    assert "BLOCKED" in result.stdout
    assert "MEASUREMENT_STACK_HIL_RESULT: BLOCKED" in result.stdout
    assert "no HTTP mutation, erase, delete, or flash" in result.stdout


def test_replay_fails_on_low_hwm_or_stack_overflow(tmp_path: Path):
    path = tmp_path / "measurement-stack-failure.log"
    path.write_text(make_log(free=512, failure="***ERROR*** A stack overflow in task measurement_ser has been detected."), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "STACK_HWM safety margin" in result.stdout
    assert "FAIL" in result.stdout
    assert "Panic/watchdog/brownout detection" in result.stdout
    assert "MEASUREMENT_STACK_HIL_RESULT: FAIL" in result.stdout


def test_live_runner_requires_dedicated_object_and_does_not_flash_by_default():
    source = RUNNER.read_text(encoding="utf-8")
    control = (REPO_ROOT / "components" / "web_server" / "web_api_control.c").read_text(encoding="utf-8")
    web_cmake = (REPO_ROOT / "components" / "web_server" / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "ExpectedObjectNamePrefix = \"HIL_STACK_TEST\"" in source
    assert "pre-created dedicated HIL object" in source
    assert 'action = "measurement_start"' in source
    assert 'action = "measurement_stop"' in source
    assert "if ($Flash)" in source
    assert "No flash requested; observing the already-installed instrumented firmware." in source
    assert "session delete/erase was issued" in source
    assert 'strcmp(action, "measurement_start") == 0' in control
    assert 'return measurement_service_stop_async();' in control
    assert "measurement_service" in web_cmake


def test_guided_workflow_steps_block_measurement_start():
    guided = ("CALIB_RESULT", "PLACEMENT", "GPS_CHOICE", "GPS_PENDING", "LAYOUT_READY",
              "RANGING", "RANGING_RESULT", "SCAN_CHOICE", "SCANNING")
    source = RUNNER.read_text(encoding="utf-8")
    assert '$guidedSteps = @(' in source
    for step in guided:
        assert f'"{step}"' in source
    assert 'if ($step -in $guidedSteps)' in source
    assert "Confirm placement, GPS choice, layout calculation, and scan choice" in source
