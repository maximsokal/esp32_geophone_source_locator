"""Replay/parser contracts for the storage/SPI latency HIL runner.

These tests never exercise physical SPI2, SD, TFT DMA, FreeRTOS scheduling,
acquisition health, or recorder timing. They only prove that the PowerShell
parser and numerical gate classify already-captured telemetry correctly.
"""

import json
import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_storage_spi_hil.ps1"


THRESHOLD_ARGS = [
    "-MaxMutexWaitUs", "100",
    "-MaxTftQuiesceUs", "1000",
    "-MaxTftResumeUs", "100",
    "-MaxSpiEndElapsedUs", "150",
    "-MaxRecorderWindowUs", "5000",
    "-MaxRecorderFwriteUs", "12000",
    "-MaxRecorderOperationUs", "15000",
]


def status_sample(
    *,
    quality: str = "HEALTHY",
    stream_state: str = "NORMAL",
    suspicious: bool = False,
    stream_stuck: bool = False,
    sample_rate_hz: int = 500,
    quality_window_frames: int = 500,
    consecutive_failures: int = 0,
    signal_quality: str = "HEALTHY",
) -> dict:
    return {
        "acquisition": {
            "sample_rate_hz": sample_rate_hz,
            "error_counters_available": True,
            "consecutive_failures": consecutive_failures,
            "serial": {
                "quality": quality,
                "stream_state": stream_state,
                "suspicious": suspicious,
                "stream_stuck": stream_stuck,
                "quality_window_frames": quality_window_frames,
            },
            "signal": {"quality": signal_quality},
        }
    }


def write_health(path: Path, *samples: dict) -> Path:
    evidence = {
        "samples": [
            {"phase": "preflight" if index == 0 else f"sample-{index}", "status": sample}
            for index, sample in enumerate(samples)
        ]
    }
    path.write_text(json.dumps(evidence), encoding="utf-8")
    return path


def make_log(
    *,
    mutex: int = 50,
    quiesce: int = 500,
    resume: int = 30,
    end_elapsed: int = 40,
    requested: int = 256,
    written: int = 256,
    windows: int = 1,
    max_window: int = 4000,
    fwrite: int = 9000,
    operation: int = 10000,
    storage_result: int = 0,
    recorder_result: int = 0,
    stop_stage: str = "complete",
    stop_error: str = "ESP_OK",
    dropped_frames: int = 0,
    include_stop: bool = True,
    diagnostic_channels: tuple[int, ...] = tuple(range(1, 9)),
    acquisition_failure: bool = False,
    extra: str = "",
) -> str:
    lines = [
        f"I SPI_BUS: STORAGE_SPI_TIMING stage=begin mutex_wait_us={mutex} tft_quiesce_us={quiesce} result={storage_result}",
        f"I SPI_BUS: STORAGE_SPI_TIMING stage=end tft_resume_us={resume} elapsed_us={end_elapsed}",
        (
            "I SEISMIC_RECORDER_FS: RECORDER_IO_TIMING op=fwrite "
            f"requested={requested} written={written} windows={windows} "
            f"max_window_us={max_window} fwrite_us={fwrite} "
            f"operation_us={operation} result={recorder_result}"
        ),
    ]

    for channel in diagnostic_channels:
        lines.append(
            f"I AD7606_DIAG: CH{channel} n=500 mean=+0.000mV rms=0.100mV p2p=0.500mV peak=0.250mV min=-0.250mV max=+0.250mV sat=0 VAR"
        )

    if acquisition_failure:
        lines.append("W AD7606_ACQ: Read failed: ESP_ERR_TIMEOUT (check BUSY/CONVST/SPI3 wiring)")

    if include_stop:
        lines.append(
            "I SEISMIC_RECORDER: STOP_COMPLETE "
            f"stage={stop_stage} error_stage=none stage_error=ESP_OK "
            f"session=SESSION_0001 samples=1000 drops={dropped_frames} err={stop_error}"
        )

    if extra:
        lines.append(extra)
    return "\n".join(lines) + "\n"


def powershell_command() -> str:
    executable = shutil.which("powershell.exe") or shutil.which("pwsh")
    if executable is None:
        raise RuntimeError("PowerShell is required for storage/SPI runner tests")
    return executable


def run_replay(path: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    executable = powershell_command()
    arguments = [executable, "-NoLogo", "-NoProfile"]
    if executable.lower().endswith("powershell.exe"):
        arguments.extend(["-ExecutionPolicy", "Bypass"])
    arguments.extend(
        [
            "-File",
            str(RUNNER),
            "-LogPath",
            str(path),
            *THRESHOLD_ARGS,
            *extra,
        ]
    )
    return subprocess.run(
        arguments,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def test_replay_parses_valid_telemetry_without_claiming_physical_hil(tmp_path: Path):
    path = tmp_path / "storage-spi-pass.log"
    path.write_text(make_log(), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "samples storage_begin=1 storage_end=1 recorder_fwrite=1 recorder_stop=1 acquisition_diag_channels=8" in result.stdout
    assert "mutex_wait_us=50us limit=100us" in result.stdout
    assert "max_window_us=4000us limit=5000us" in result.stdout
    assert "recorder_dropped_frames=0 expected=0" in result.stdout
    assert "acquisition_failure_markers=0 expected=0" in result.stdout
    assert "acquisition_diag_channels=1,2,3,4,5,6,7,8 required=8" in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: PASS" in result.stdout
    assert "AD7606_TRANSPORT_HEALTH: BLOCKED" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout
    assert "never proves physical SPI2" in result.stdout


def test_replay_consumes_healthy_saved_health_without_network_and_accepts_quiet(tmp_path: Path):
    path = tmp_path / "storage-spi-health-pass.log"
    path.write_text(make_log(), encoding="utf-8")
    health = write_health(
        tmp_path / "health.json",
        status_sample(stream_state="QUIET", signal_quality="CLIPPED"),
        status_sample(),
    )

    result = run_replay(path, "-HealthEvidencePath", str(health))

    assert result.returncode == 1
    assert "AD7606_TRANSPORT_HEALTH: PASS" in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: PASS" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout
    assert "REPLAY_LIMITATION" in result.stdout


def test_replay_health_fault_is_fail_even_if_final_sample_recovers(tmp_path: Path):
    path = tmp_path / "storage-spi-health-fail.log"
    path.write_text(make_log(), encoding="utf-8")
    health = write_health(
        tmp_path / "health-fail.json",
        status_sample(),
        status_sample(quality="FAULT", stream_state="SUSPICIOUS", suspicious=True),
        status_sample(),
    )

    result = run_replay(path, "-HealthEvidencePath", str(health))

    assert result.returncode == 1
    assert "AD7606_TRANSPORT_HEALTH: FAIL" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_replay_immature_health_remains_blocked(tmp_path: Path):
    path = tmp_path / "storage-spi-health-immature.log"
    path.write_text(make_log(), encoding="utf-8")
    health = write_health(
        tmp_path / "health-immature.json",
        status_sample(quality="UNKNOWN", quality_window_frames=100),
    )

    result = run_replay(path, "-HealthEvidencePath", str(health))

    assert result.returncode == 1
    assert "AD7606_TRANSPORT_HEALTH: BLOCKED" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_uses_reported_rate_for_health_and_uart_window_maturity(tmp_path: Path):
    serial_fault = (
        "I AD7606_DIAG: AD7606_SERIAL_DIAG: transport_quality=FAULT "
        "signal_quality=HEALTHY stream_state=STUCK stream_stuck=yes "
        "window=500 serial_suspicious=yes"
    )
    path = tmp_path / "storage-spi-high-rate.log"
    path.write_text(make_log(extra=serial_fault), encoding="utf-8")
    immature = write_health(
        tmp_path / "health-high-rate-immature.json",
        status_sample(sample_rate_hz=20_000, quality_window_frames=19_999),
    )

    blocked = run_replay(path, "-HealthEvidencePath", str(immature))
    assert "19999/20000 frames, one second at 20000Hz" in blocked.stdout
    assert "AD7606_TRANSPORT_FAULT:" not in blocked.stdout

    mature = write_health(
        tmp_path / "health-high-rate-mature.json",
        status_sample(sample_rate_hz=20_000, quality_window_frames=20_000),
    )
    healthy = run_replay(path, "-HealthEvidencePath", str(mature))
    assert "AD7606_TRANSPORT_HEALTH: PASS" in healthy.stdout
    assert "AD7606_TRANSPORT_FAULT:" not in healthy.stdout


def test_replay_mature_uart_fault_is_sticky_with_healthy_pre_post_status(tmp_path: Path):
    serial_fault = (
        "I AD7606_DIAG: AD7606_SERIAL_DIAG: rx=00 00 class=ALL_00 "
        "transport_quality=FAULT signal_quality=HEALTHY stream_state=SUSPICIOUS "
        "stream_stuck=yes busy=0 samples=500 streak=3 repeated_streak=0 all_zero=500 "
        "all_ff=0 identical=0 varied=0 digital_interference=0 signal_rail_hits=0 "
        "signal_clips=0 window=500 transport_bad=500 all_zero=500 all_ff=0 identical=0 "
        "repeated=0 digital=0 signal_rail=0 signal_clip=0 serial_suspicious=yes"
    )
    path = tmp_path / "storage-spi-uart-fault.log"
    path.write_text(make_log(extra=serial_fault), encoding="utf-8")
    health = write_health(tmp_path / "health-ok.json", status_sample(), status_sample())

    result = run_replay(path, "-HealthEvidencePath", str(health))

    assert result.returncode == 1
    assert "AD7606_TRANSPORT_FAULT:" in result.stdout
    assert "AD7606_TRANSPORT_HEALTH: FAIL" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_replay_accepts_exact_threshold_boundary(tmp_path: Path):
    path = tmp_path / "storage-spi-boundary.log"
    path.write_text(
        make_log(
            mutex=100,
            quiesce=1000,
            resume=100,
            end_elapsed=150,
            max_window=5000,
            fwrite=12000,
            operation=15000,
        ),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "THRESHOLD_VIOLATION:" not in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: PASS" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_rejects_each_latency_over_limit(tmp_path: Path):
    cases = {
        "mutex": {"mutex": 101, "needle": "mutex_wait_us=101us exceeds limit=100us"},
        "quiesce": {"quiesce": 1001, "needle": "tft_quiesce_us=1001us exceeds limit=1000us"},
        "resume": {"resume": 101, "needle": "tft_resume_us=101us exceeds limit=100us"},
        "spi_end": {"end_elapsed": 151, "needle": "spi_end_elapsed_us=151us exceeds limit=150us"},
        "window": {"max_window": 5001, "needle": "max_window_us=5001us exceeds limit=5000us"},
        "fwrite": {"fwrite": 12001, "needle": "fwrite_us=12001us exceeds limit=12000us"},
        "operation": {"operation": 15001, "needle": "recorder_operation_us=15001us exceeds limit=15000us"},
    }

    for name, case in cases.items():
        kwargs = {key: value for key, value in case.items() if key != "needle"}
        path = tmp_path / f"storage-spi-{name}.log"
        path.write_text(make_log(**kwargs), encoding="utf-8")
        result = run_replay(path)

        assert result.returncode == 1
        assert case["needle"] in result.stdout
        assert "STORAGE_SPI_TELEMETRY_RESULT: FAIL" in result.stdout
        assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_storage_error_short_write_and_zero_windows(tmp_path: Path):
    path = tmp_path / "storage-spi-integrity.log"
    path.write_text(
        make_log(
            storage_result=-1,
            requested=256,
            written=128,
            windows=0,
            recorder_result=-1,
        ),
        encoding="utf-8",
    )

    result = run_replay(path)

    assert result.returncode == 1
    assert "storage begin result=-1" in result.stdout
    assert "recorder fwrite result=-1" in result.stdout
    assert "recorder short write requested=256 written=128" in result.stdout
    assert "recorder fwrite reported zero storage windows" in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: FAIL" in result.stdout


def test_replay_rejects_dropped_frames_or_abnormal_recorder_stop(tmp_path: Path):
    cases = [
        (make_log(dropped_frames=3), "recorder dropped_frames=3 expected=0"),
        (make_log(stop_stage="error", stop_error="ESP_FAIL"), "recorder stop stage=error expected=complete"),
        (make_log(stop_stage="complete", stop_error="ESP_FAIL"), "recorder stop err=ESP_FAIL expected=ESP_OK"),
    ]

    for index, (content, needle) in enumerate(cases):
        path = tmp_path / f"storage-spi-recorder-health-{index}.log"
        path.write_text(content, encoding="utf-8")
        result = run_replay(path)

        assert result.returncode == 1
        assert needle in result.stdout
        assert "STORAGE_SPI_TELEMETRY_RESULT: FAIL" in result.stdout
        assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_acquisition_failure_marker(tmp_path: Path):
    path = tmp_path / "storage-spi-acquisition-fail.log"
    path.write_text(make_log(acquisition_failure=True), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "acquisition failure markers=1 expected=0" in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: FAIL" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_replay_missing_or_malformed_telemetry_is_blocked_not_pass(tmp_path: Path):
    logs = {
        "missing": "I APP: unrelated log line\n",
        "malformed": (
            "I SPI_BUS: STORAGE_SPI_TIMING stage=begin mutex_wait_us=? tft_quiesce_us=500 result=0\n"
            "I SEISMIC_RECORDER_FS: RECORDER_IO_TIMING op=fwrite requested=256 written=256 windows=1 max_window_us=oops fwrite_us=10 operation_us=10 result=0\n"
        ),
        "no-stop": make_log(include_stop=False),
        "not-all-diagnostic-channels": make_log(diagnostic_channels=(1, 2, 3, 4)),
    }

    for name, content in logs.items():
        path = tmp_path / f"storage-spi-{name}.log"
        path.write_text(content, encoding="utf-8")
        result = run_replay(path)

        assert result.returncode == 1
        assert "MISSING_TELEMETRY:" in result.stdout
        assert "STORAGE_SPI_TELEMETRY_RESULT: BLOCKED" in result.stdout
        assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_rejects_target_failure_marker(tmp_path: Path):
    path = tmp_path / "storage-spi-panic.log"
    path.write_text(make_log(extra="Guru Meditation Error: Core 0 panic'ed"), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "TARGET_FAILURE: panic/watchdog/brownout/assert marker detected" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: FAIL" in result.stdout


def test_runner_is_passive_and_health_gated_by_contract():
    source = RUNNER.read_text(encoding="utf-8")
    contract = (REPO_ROOT / "tools" / "storage_spi_contract.ps1").read_text(encoding="utf-8")

    assert '[string]$LogPort = "COM6"' in source
    assert '[string]$BaseUrl = ""' in source
    assert '[string]$HealthEvidencePath = ""' in source
    assert '"-Action", "monitor"' in source
    assert "This runner never flashes firmware" in source
    assert '"-Action", "flash"' not in source
    assert '"{0}/api/status" -f $base' in source
    assert "quality_window_frames" in source
    assert "stream_state" in source
    assert "serial_suspicious" in source
    assert "HEALTH_EVIDENCE" in source
    assert "STORAGE_SPI_HIL_RESULT" in source
    assert "REPLAY_LIMITATION" in source
    assert "MinimumAcquisitionDiagnosticChannels = 8" in source
    assert "STOP_COMPLETE" in contract
    assert "AD7606_DIAG" in contract
    assert "DroppedFrames" in contract
    assert "AcquisitionFailureCount" in contract
    assert "MaxRecorderWindowUs" in contract
    assert "MaxRecorderFwriteUs" in contract
