"""Replay/parser contracts for the storage/SPI latency HIL runner.

These tests never exercise physical SPI2, SD, TFT DMA, FreeRTOS scheduling, or
recorder timing.  They only prove that the PowerShell parser and numerical
threshold gate classify already-captured telemetry correctly.
"""

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
    if extra:
        lines.append(extra)
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
            *THRESHOLD_ARGS,
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


def test_replay_parses_valid_telemetry_without_claiming_physical_hil(tmp_path: Path):
    path = tmp_path / "storage-spi-pass.log"
    path.write_text(make_log(), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "samples storage_begin=1 storage_end=1 recorder_fwrite=1" in result.stdout
    assert "mutex_wait_us=50us limit=100us" in result.stdout
    assert "max_window_us=4000us limit=5000us" in result.stdout
    assert "STORAGE_SPI_TELEMETRY_RESULT: PASS" in result.stdout
    assert "STORAGE_SPI_HIL_RESULT: BLOCKED" in result.stdout
    assert "never proves physical SPI2" in result.stdout


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


def test_replay_missing_or_malformed_telemetry_is_blocked_not_pass(tmp_path: Path):
    logs = {
        "missing": "I APP: unrelated log line\n",
        "malformed": (
            "I SPI_BUS: STORAGE_SPI_TIMING stage=begin mutex_wait_us=? tft_quiesce_us=500 result=0\n"
            "I SEISMIC_RECORDER_FS: RECORDER_IO_TIMING op=fwrite requested=256 written=256 windows=1 max_window_us=oops fwrite_us=10 operation_us=10 result=0\n"
        ),
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


def test_runner_is_passive_and_never_flashes_by_contract():
    source = RUNNER.read_text(encoding="utf-8")
    contract = (REPO_ROOT / "tools" / "storage_spi_contract.ps1").read_text(encoding="utf-8")

    assert '[string]$LogPort = "COM6"' in source
    assert '"-Action", "monitor"' in source
    assert "This runner never flashes firmware" in source
    assert '"-Action", "flash"' not in source
    assert "STORAGE_SPI_HIL_RESULT" in source
    assert "REPLAY_LIMITATION" in source
    assert "STORAGE_SPI_TIMING" in contract
    assert "RECORDER_IO_TIMING" in contract
    assert "MaxRecorderWindowUs" in contract
    assert "MaxRecorderFwriteUs" in contract
