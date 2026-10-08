"""Replay contracts for the opt-in keypad timing HIL runner."""

import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run_keypad_timing_hil.ps1"


def timing_log(source: str, backend_marker: str) -> str:
    return f"""\
I KEYPAD: KEYPAD_TIMING_READY source={source} button_period_ms=10 probe_period_us=10000 resolution_us=4
{backend_marker}
I KEYPAD: KEYPAD_ADC_TIMING: source={source} window_ms=1000 attempts=100 snapshots=100 busy=0 errors=0 min_us=12 avg_us=18 p95_us=23 p95_overflow=0 p95_censored_above_us=0 max_us=31 resolution_us=4 overflow=0
I KEYPAD: KEYPAD_TIMER_TIMING: period_us=10000 window_ms=1000 samples=100 min_lateness_us=0 avg_lateness_us=2 p95_lateness_us=7 p95_overflow=0 p95_censored_above_us=0 max_lateness_us=11 late_count=40 overrun_count=0 resolution_us=4 overflow=0
I KEYPAD: KEYPAD_ADC_TIMING: source={source} window_ms=1000 attempts=101 snapshots=101 busy=0 errors=0 min_us=11 avg_us=19 p95_us=27 p95_overflow=0 p95_censored_above_us=0 max_us=35 resolution_us=4 overflow=0
I KEYPAD: KEYPAD_TIMER_TIMING: period_us=10000 window_ms=1000 samples=100 min_lateness_us=0 avg_lateness_us=3 p95_lateness_us=11 p95_overflow=0 p95_censored_above_us=0 max_lateness_us=17 late_count=50 overrun_count=0 resolution_us=4 overflow=0
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
            "-MinimumWindows",
            "2",
            "-MinimumSnapshots",
            "2",
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


def test_ad7606_replay_reports_adc_and_timer_aggregates(tmp_path: Path):
    path = tmp_path / "ad7606.log"
    path.write_text(
        timing_log(
            "AD7606",
            "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
        ),
        encoding="utf-8",
    )

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Selected acquisition backend" in result.stdout
    assert "Keypad ADC snapshot timing" in result.stdout
    assert "snapshots=201" in result.stdout
    assert "p95_us=27" in result.stdout
    assert "ESP_TIMER_TASK dispatch timing" in result.stdout
    assert "p95_lateness_us=11" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout
    assert "PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED" in result.stdout


def test_default_rate_accepts_reported_20khz_and_override_can_reject_it(tmp_path: Path):
    path = tmp_path / "ad7606-20khz.log"
    path.write_text(
        timing_log(
            "AD7606",
            "I AD7606_ACQ: Started: 20000 Hz, 8 channels, SPI3 dedicated, core=1",
        ),
        encoding="utf-8",
    )

    result = run_replay(path, "-Source", "AD7606")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "backend marker reports 20000Hz" in result.stdout

    mismatch = run_replay(path, "-Source", "AD7606", "-ExpectedSampleRateHz", "500")
    assert mismatch.returncode == 1
    assert "reported 20000Hz, expected 500Hz" in mismatch.stdout


def test_overflowed_p95_is_reported_as_censored_data(tmp_path: Path):
    path = tmp_path / "overflow.log"
    content = timing_log(
        "AD7606",
        "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
    )
    content = content.replace(
        "p95_us=23 p95_overflow=0 p95_censored_above_us=0 max_us=31 resolution_us=4 overflow=0",
        "p95_us=0 p95_overflow=1 p95_censored_above_us=2048 max_us=5100 resolution_us=4 overflow=3",
        1,
    )
    content = content.replace(
        "p95_lateness_us=7 p95_overflow=0 p95_censored_above_us=0 max_lateness_us=11 late_count=40 overrun_count=0 resolution_us=4 overflow=0",
        "p95_lateness_us=0 p95_overflow=1 p95_censored_above_us=2048 max_lateness_us=5100 late_count=40 overrun_count=0 resolution_us=4 overflow=3",
        1,
    )
    path.write_text(content, encoding="utf-8")

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "p95_us=0 p95_overflow=1 p95_censored_above_us=2048" in result.stdout
    assert "p95_lateness_us=0 p95_overflow=1 p95_censored_above_us=2048" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout
    assert "PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED" in result.stdout


def test_last_normal_bucket_p95_is_valid_telemetry(tmp_path: Path):
    path = tmp_path / "normal-boundary.log"
    content = timing_log(
        "AD7606",
        "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
    ).replace(
        "p95_us=23 p95_overflow=0 p95_censored_above_us=0 max_us=31 resolution_us=4 overflow=0",
        "p95_us=2047 p95_overflow=0 p95_censored_above_us=0 max_us=2047 resolution_us=4 overflow=0",
        1,
    )
    path.write_text(content, encoding="utf-8")

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "p95_us=2047 p95_overflow=0 p95_censored_above_us=0" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout
    assert "PHYSICAL_CONDITION: NOT_CONFIRMED" in result.stdout
    assert "KEYPAD_TIMING_RUN_RESULT: PASS" in result.stdout


def test_small_overflow_tail_does_not_censor_valid_p95(tmp_path: Path):
    path = tmp_path / "small-overflow-tail.log"
    content = timing_log(
        "AD7606",
        "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
    ).replace(
        "p95_us=23 p95_overflow=0 p95_censored_above_us=0 max_us=31 resolution_us=4 overflow=0",
        "p95_us=2047 p95_overflow=0 p95_censored_above_us=0 max_us=5100 resolution_us=4 overflow=1",
        1,
    )
    path.write_text(content, encoding="utf-8")

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "p95_us=2047 p95_overflow=0 p95_censored_above_us=0" in result.stdout
    assert "overflow=1" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout


def test_nonzero_adc_errors_fail_telemetry_result(tmp_path: Path):
    path = tmp_path / "errors.log"
    content = timing_log(
        "AD7606",
        "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
    ).replace("errors=0", "errors=1", 1)
    path.write_text(content, encoding="utf-8")

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 1
    assert "non-zero ADC read errors are not acceptable" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: FAIL" in result.stdout
    assert "PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED" in result.stdout


def test_replay_rejects_malformed_timing_marker(tmp_path: Path):
    path = tmp_path / "malformed.log"
    path.write_text(
        timing_log(
            "AD7606",
            "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
        ).replace("p95_us=23", "p95_us=9999", 1),
        encoding="utf-8",
    )

    result = run_replay(path, "-Source", "AD7606")

    assert result.returncode == 1
    assert "p95_us is outside the measured range" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: FAIL" in result.stdout


def test_replay_cannot_attest_frequent_physical_key_activity(tmp_path: Path):
    path = tmp_path / "attestation.log"
    path.write_text(
        timing_log(
            "AD7606",
            "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
        ),
        encoding="utf-8",
    )

    result = run_replay(
        path,
        "-Source",
        "AD7606",
        "-ConfirmFrequentKeypadActivity",
    )

    assert result.returncode == 1
    assert "cannot be used with -LogPath" in result.stdout
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout
    assert "PHYSICAL_CONDITION: NOT_CONFIRMED" in result.stdout
    assert "KEYPAD_TIMING_RUN_RESULT: BLOCKED" in result.stdout


def test_required_physical_activity_blocks_run_without_changing_telemetry_result(
    tmp_path: Path,
):
    path = tmp_path / "required-activity.log"
    path.write_text(
        timing_log(
            "AD7606",
            "I AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1",
        ),
        encoding="utf-8",
    )

    result = run_replay(
        path,
        "-Source",
        "AD7606",
        "-RequireFrequentKeypadActivity",
    )

    assert result.returncode == 1
    assert "KEYPAD_TIMING_TELEMETRY_RESULT: PASS" in result.stdout
    assert "PHYSICAL_CONDITION: NOT_CONFIRMED" in result.stdout
    assert "KEYPAD_TIMING_RUN_RESULT: BLOCKED" in result.stdout
