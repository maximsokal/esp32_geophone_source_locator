"""Replay contract for the explicit production pipeline HIL runner."""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import threading
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_pipeline_hil.ps1"


GOOD_LOG = """\
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
I AD7606_DRIVER: GPIO configured: RESET=2 CONVST=42 BUSY=47 CS=41
I AD7606_DRIVER: Initializing dedicated SPI3: MISO=39 SCLK=40
I AD7606_DRIVER: SPI3 initialized successfully: DMA=auto (SPI_DMA_CH_AUTO)
I AD7606_DRIVER: Self-test conversion + SPI read OK
I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1
I AD7606_DIAG: CH1 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH2 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH3 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH4 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH5 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH6 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH7 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
I AD7606_DIAG: CH8 n=500 mean=0 rms=1 p2p=2 peak=1 min=-1 max=1 sat=0 VAR
"""


def run_replay(path: Path, *extra_args: str) -> subprocess.CompletedProcess[str]:
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
            *extra_args,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )


def run_api_fixture(
    tmp_path: Path,
    seismic_payload: dict,
    *,
    sample_rate_hz: int = 500,
    log_text: str = GOOD_LOG,
    stale_tasks: tuple[str, ...] = (),
    future_tasks: tuple[str, ...] = (),
    backwards_clock: bool = False,
    max_age_ms: int | None = None,
) -> subprocess.CompletedProcess[str]:
    log_path = tmp_path / "good.log"
    log_path.write_text(log_text, encoding="utf-8")
    polls = {"seismic": 0, "status": 0}

    def status_payload(index: int) -> dict:
        now_ms = 1000 - index * 100 if backwards_clock else 1000 + index * 100
        tasks = {
            name: {
                "observed": True,
                "heartbeat": 10 + index,
                "progress": 500 + index if name == "acquisition" else 80 + index if name == "recorder" else index,
                "last_heartbeat_ms": (
                    0
                    if name in stale_tasks
                    else now_ms + 1
                    if name in future_tasks
                    else now_ms
                ),
            }
            for name in ("acquisition", "recorder", "ui")
        }
        return {
            "acquisition": {
                "running": True,
                "hardware_ready": True,
                "source": "AD7606",
                "sample_rate_hz": sample_rate_hz,
                "last_error": 0,
                "error_counters_available": True,
                "read_errors": 0,
                "consecutive_failures": 0,
            },
            "runtime": {"now_ms": now_ms, "tasks": tasks},
        }

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - stdlib protocol name
            if self.path.startswith("/api/seismic"):
                payload = dict(seismic_payload)
                payload["total_frames"] += polls["seismic"]
                polls["seismic"] += 1
            elif self.path.startswith("/api/status"):
                payload = status_payload(polls["status"])
                polls["status"] += 1
            else:
                self.send_response(404)
                self.end_headers()
                return
            encoded = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def log_message(self, format: str, *args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        extra_args = [
            "-BaseUrl",
            f"http://127.0.0.1:{server.server_port}",
            "-PollIntervalMs",
            "100",
        ]
        if max_age_ms is not None:
            extra_args.extend(["-MaxTaskHeartbeatAgeMs", str(max_age_ms)])
        return run_replay(log_path, *extra_args)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def test_replay_passes_without_serial_ports_and_never_flashes(tmp_path: Path):
    (tmp_path / "good.log").write_text(GOOD_LOG, encoding="utf-8")
    result = run_replay(tmp_path / "good.log")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Live serial preflight" in result.stdout
    assert "SKIPPED   Replay mode from -LogPath" in result.stdout
    assert "Production flash" in result.stdout
    assert "PIPELINE_HIL_RESULT: PASS" in result.stdout


def test_replay_uses_reported_20khz_rate_by_default_and_supports_an_override(tmp_path: Path):
    path = tmp_path / "20khz.log"
    path.write_text(GOOD_LOG.replace("Started: 500 Hz", "Started: 20000 Hz"), encoding="utf-8")

    result = run_replay(path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Startup marker reports 20000Hz" in result.stdout

    mismatch = run_replay(path, "-ExpectedSampleRateHz", "500")
    assert mismatch.returncode == 1
    assert "Reported rate is 20000Hz, expected 500Hz" in mismatch.stdout


def test_replay_reports_read_failure(tmp_path: Path):
    path = tmp_path / "bad.log"
    path.write_text(GOOD_LOG + "E AD7606_ACQ: Read failed: timeout\n", encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "Acquisition read failures" in result.stdout
    assert "FAIL" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_wrong_started_sample_rate(tmp_path: Path):
    path = tmp_path / "wrong-rate.log"
    path.write_text(GOOD_LOG.replace("Started: 500 Hz", "Started: 100 Hz"), encoding="utf-8")

    result = run_replay(path, "-ExpectedSampleRateHz", "500")

    assert result.returncode == 1
    assert "AD7606 acquisition started" in result.stdout
    assert "Reported rate is 100Hz, expected 500Hz" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_api_pipeline_derives_20khz_expectation_from_reported_rates(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 20000,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1001, 1002],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in range(1, 5)],
        },
        sample_rate_hz=20000,
        log_text=GOOD_LOG.replace("Started: 500 Hz", "Started: 20000 Hz"),
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Startup marker reports 20000Hz" in result.stdout
    assert "rate=20000Hz" in result.stdout
    assert "PIPELINE_HIL_RESULT: PASS" in result.stdout


def test_replay_rejects_missing_dma_marker(tmp_path: Path):
    path = tmp_path / "missing-dma-marker.log"
    path.write_text(GOOD_LOG.replace(": DMA=auto (SPI_DMA_CH_AUTO)", ""), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "AD7606 SPI3 initialization" in result.stdout
    assert "Required marker was not observed" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_api_seismic_requires_exact_channel_ids(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 500,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1002, 1004],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in (1, 2, 3, 8)],
        },
    )

    assert result.returncode == 1
    assert "channel IDs=[1,2,3,8], expected [1,2,3,4]" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_api_seismic_rejects_large_timestamp_jitter(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 500,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1002, 1100],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in range(1, 5)],
        },
    )

    assert result.returncode == 1
    assert "timestamp interval at index 2" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_saved_lod_runner_checks_against_the_reported_rate_by_default():
    runner = (REPO_ROOT / "tools" / "run_saved_lod_hil.ps1").read_text(encoding="utf-8")

    assert "[int]$ExpectedSampleRateHz = 0" in runner
    assert "$reportedSampleRateHz = [int]$status.acquisition.sample_rate_hz" in runner
    assert "[int]$saved.sample_rate_hz -ne $reportedSampleRateHz" in runner


def test_api_status_rejects_stale_runtime_heartbeat(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 500,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1002, 1004],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in range(1, 5)],
        },
        stale_tasks=("ui",),
        max_age_ms=100,
    )

    assert result.returncode == 1
    assert "heartbeat too old" in result.stdout
    assert "ui" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_api_status_rejects_future_runtime_heartbeat(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 500,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1002, 1004],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in range(1, 5)],
        },
        future_tasks=("ui",),
    )

    assert result.returncode == 1
    assert "heartbeat timestamp is in the future" in result.stdout
    assert "ui" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_api_status_rejects_backwards_device_clock(tmp_path: Path):
    result = run_api_fixture(
        tmp_path,
        {
            "source": "AD7606",
            "sample_rate_hz": 500,
            "count": 3,
            "total_frames": 100,
            "timestamps_ms": [1000, 1002, 1004],
            "channels": [{"id": channel_id, "samples_uv": [1, 2, 3]} for channel_id in range(1, 5)],
        },
        backwards_clock=True,
    )

    assert result.returncode == 1
    assert "runtime.now_ms moved backwards" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_require_full_pipeline_blocks_before_live_preflight(tmp_path: Path):
    (tmp_path / "good.log").write_text(GOOD_LOG, encoding="utf-8")

    result = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(RUNNER),
            "-RequireFullPipeline",
            "-TimeoutSeconds",
            "5",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=15,
    )

    assert result.returncode == 1
    assert "Full pipeline prerequisites" in result.stdout
    assert "stopping before serial preflight, monitor, or flash" in result.stdout
    assert "Live serial preflight                  PASS" not in result.stdout
    assert "Starting production monitor" not in result.stdout
    assert "Production flash                  PASS" not in result.stdout
    assert "PIPELINE_HIL_RESULT: BLOCKED" in result.stdout


def test_replay_reports_missing_channel_without_waiting_for_live_timeout(tmp_path: Path):
    path = tmp_path / "missing-channel.log"
    path.write_text(GOOD_LOG.replace("I AD7606_DIAG: CH8", "I AD7606_DIAG: CH4"), encoding="utf-8")

    started = time.monotonic()
    result = run_replay(path)
    elapsed = time.monotonic() - started

    assert result.returncode == 1
    assert elapsed < 5
    assert "AD7606 channel diagnostics" in result.stdout
    assert "missing: 8" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_replay_rejects_zero_sample_channel_diagnostic(tmp_path: Path):
    path = tmp_path / "zero-channel.log"
    path.write_text(GOOD_LOG.replace("I AD7606_DIAG: CH8 n=500", "I AD7606_DIAG: CH8 n=0"), encoding="utf-8")

    result = run_replay(path)

    assert result.returncode == 1
    assert "AD7606 channel diagnostics" in result.stdout
    assert "missing: 8" in result.stdout
    assert "PIPELINE_HIL_RESULT: FAIL" in result.stdout


def test_replay_never_accepts_physical_attestations(tmp_path: Path):
    path = tmp_path / "attestation-replay.log"
    path.write_text(GOOD_LOG, encoding="utf-8")

    result = run_replay(
        path,
        "-ConfirmTftRendering",
        "-ConfirmKeypadResponsive",
        "-ConfirmPowerButtonResponsive",
        "-ConfirmPowerButtonTiming",
    )

    assert result.returncode == 1
    assert result.stdout.count("cannot be used with -LogPath") == 4
    assert "Physical TFT rendering                 BLOCKED" in result.stdout
    assert "Physical keypad responsiveness         BLOCKED" in result.stdout
    assert "Physical power-button responsiveness   BLOCKED" in result.stdout
    assert "Power button raw timing" in result.stdout
    assert "PIPELINE_HIL_RESULT: BLOCKED" in result.stdout


def test_api_status_confirms_all_runtime_tasks_advance(tmp_path: Path):
    (tmp_path / "good.log").write_text(GOOD_LOG, encoding="utf-8")
    polls = {"seismic": 0, "status": 0}

    def status_payload(index: int) -> dict:
        tasks = {}
        for name in ("acquisition", "recorder", "ui"):
            tasks[name] = {
                "observed": True,
                "heartbeat": 10 + index,
                "progress": (500 + index if name == "acquisition" else 80 + index if name == "recorder" else index),
                "last_heartbeat_ms": 1000 + index * 100,
            }
        return {
            "acquisition": {
                "running": True,
                "hardware_ready": True,
                "source": "AD7606",
                "sample_rate_hz": 500,
                "last_error": 0,
                "error_counters_available": True,
                "read_errors": 0,
                "consecutive_failures": 0,
            },
            "runtime": {"now_ms": 1000 + index * 100, "tasks": tasks},
        }

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - stdlib protocol name
            if self.path.startswith("/api/seismic"):
                index = polls["seismic"]
                polls["seismic"] += 1
                payload = {
                    "source": "AD7606",
                    "sample_rate_hz": 500,
                    "count": 2,
                    "total_frames": 100 + index,
                    "timestamps_ms": [1000, 1002],
                    "channels": [
                        {"id": channel_id, "samples_uv": [1, 2]}
                        for channel_id in range(1, 5)
                    ],
                }
            elif self.path.startswith("/api/status"):
                index = polls["status"]
                polls["status"] += 1
                payload = status_payload(index)
            else:
                self.send_response(404)
                self.end_headers()
                return
            encoded = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def log_message(self, format: str, *args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = run_replay(
            tmp_path / "good.log",
            "-BaseUrl",
            f"http://127.0.0.1:{server.server_port}",
            "-PollIntervalMs",
            "100",
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "HTTP task responsiveness" in result.stdout
    assert any(
        line.startswith("HTTP task responsiveness") and " PASS " in line
        for line in result.stdout.splitlines()
    )
    assert "PIPELINE_HIL_RESULT: PASS" in result.stdout
