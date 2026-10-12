"""Contracts for the non-destructive WebUI/TLS memory HIL runner."""

import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_webui_memory_hil.ps1"


def make_log(*, asset_ms: int = 120, failure: str = "") -> str:
    lines = [
        "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)",
        "I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=idle free=4096 margin=1024",
        "I RUNTIME_OBSERVABILITY: RUNTIME_CPU stage=recording-active window_ms=10000 valid=1 cpu0_busy=42.0 cpu0_idle=58.0 cpu1_busy=8.0 cpu1_idle=92.0",
        "I RUNTIME_OBSERVABILITY: MEM_RUNTIME stage=recording-active internal_free=64000 internal_largest=24000 dma_free=32000 dma_largest=16000 psram_free=7000000 psram_largest=6000000 psram_dma_free=0 psram_dma_largest=0",
        "I RUNTIME_OBSERVABILITY: RUNTIME_TASK name=measurement_service core=0 priority=3 runtime_delta_us=1000 cpu=0.1 stack_free=4096",
    ]
    for path in ("/", "/assets/app.css", "/assets/app.js"):
        lines.append(f"WEBUI_ASSET path={path} elapsed_ms={asset_ms} bytes=128")
    lines.extend(
        [
            "I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=before-start free=4096 margin=1024",
            "I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=after-recorder-start free=4096 margin=1024",
            "I MEASUREMENT_SERVICE: STACK_HWM task=measurement_service stage=after-stop free=4096 margin=1024",
            "I MEASUREMENT_SERVICE: Recording stopped state=REVIEW session=S000001 samples=100 drops=0",
        ]
    )
    if failure:
        lines.append(failure)
    return "\n".join(lines) + "\n"


def run_replay(path: Path, *extra: str) -> subprocess.CompletedProcess[str]:
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
            *extra,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def test_replay_reports_target_memory_and_webui_checks_without_claiming_hil(tmp_path: Path):
    path = tmp_path / "webui-memory.log"
    path.write_text(make_log(), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "Measurement HWM safety margin" in result.stdout
    assert "Target marker RUNTIME_CPU stage" in result.stdout
    assert "WebUI asset latency" in result.stdout
    assert "PASS      Observed 3; required at least 3 asset timings." in result.stdout
    assert "All measured assets stayed below cold_root=2000ms and subsequent=1000ms." in result.stdout
    assert "MEMORY_HIL_MIN internal_free=64000 internal_largest=24000 dma_free=32000 dma_largest=16000 psram_free=7000000 psram_largest=6000000 psram_dma_free=0 psram_dma_largest=0" in result.stdout
    assert "BLOCKED" in result.stdout
    assert "WEBUI_MEMORY_HIL_RESULT: BLOCKED" in result.stdout
    assert "never flashes" in result.stdout


def test_replay_rejects_slow_asset_and_target_failure(tmp_path: Path):
    path = tmp_path / "webui-memory-failure.log"
    path.write_text(
        make_log(asset_ms=10000, failure="***ERROR*** task watchdog timeout"),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "WebUI asset latency" in result.stdout
    assert "Panic/watchdog/brownout/OOM detection" in result.stdout
    assert "FAIL" in result.stdout
    assert "WEBUI_MEMORY_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_subsequent_asset_over_one_second(tmp_path: Path):
    path = tmp_path / "webui-memory-latency.log"
    path.write_text(make_log(asset_ms=1500), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "WebUI asset latency" in result.stdout
    assert "reached 1500ms; limit=1000ms" in result.stdout
    assert "WEBUI_MEMORY_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_memory_floor_breach(tmp_path: Path):
    path = tmp_path / "webui-memory-floor.log"
    path.write_text(
        make_log().replace("internal_largest=24000", "internal_largest=7000"),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "Minimum internal largest block" in result.stdout
    assert "required >= 8192 bytes" in result.stdout
    assert "WEBUI_MEMORY_HIL_RESULT: FAIL" in result.stdout


def test_live_runner_is_dedicated_and_non_destructive_by_contract():
    source = RUNNER.read_text(encoding="utf-8")

    assert 'ExpectedObjectNamePrefix = "HIL_MEMORY_TEST"' in source
    assert "pre-created dedicated HIL object" in source
    assert 'action = "measurement_start"' in source
    assert 'action = "measurement_stop"' in source
    assert "Invoke-WebUiBatch" in source
    assert "runtime_observability_request_snapshot" in (REPO_ROOT / "components" / "web_server" / "web_server.c").read_text(encoding="utf-8")
    assert '"/assets/app.css"' in source
    assert '"/assets/app.js"' in source
    assert "RecordSeconds = 30" in source
    assert "StopTimeoutSeconds = 30" in source
    assert "MaxAssetLatencyMs = 1000" in source
    assert "MaxColdTlsLatencyMs = 2000" in source
    assert "MinimumInternalLargestFreeBlockBytes = 8192" in source
    assert "MEMORY_HIL_MIN" in source
    assert "This is a test-run bound for recorder finalization" in source
    assert "RUNTIME_CPU stage=" in source
    assert "MEM_RUNTIME stage=" in source
    assert "psram_dma_free" in source
    assert "psram_dma_largest" in source
    assert "never flashes" in source
    assert '"delete_object"' not in source
    assert "erase" in source


def test_guided_workflow_steps_block_memory_measurement_start():
    guided = ("CALIB_RESULT", "PLACEMENT", "GPS_CHOICE", "GPS_PENDING", "LAYOUT_READY",
              "RANGING", "RANGING_RESULT", "SCAN_CHOICE", "SCANNING")
    source = RUNNER.read_text(encoding="utf-8")
    assert '$guidedSteps = @(' in source
    for step in guided:
        assert f'"{step}"' in source
    assert 'if ($step -in $guidedSteps)' in source
    assert "Confirm placement, GPS choice, layout calculation, and scan choice" in source
