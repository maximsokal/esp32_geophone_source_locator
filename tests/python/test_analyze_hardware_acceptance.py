from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO_ROOT / "tools" / "analyze_hardware_acceptance.py"

_spec = importlib.util.spec_from_file_location("analyze_hardware_acceptance", RUNNER_PATH)
assert _spec is not None and _spec.loader is not None
runner = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = runner
_spec.loader.exec_module(runner)


def healthy_log(*, inherited: bool = False) -> str:
    lines = [
        "I APP: BOOT_TIMING: startup_total=3200 ms",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=START",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_PROGRESS object=OBJ_0001 index=1 total=2 removed=0 elapsed_ms=120",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_PROGRESS object=OBJ_0002 index=2 total=2 removed=0 elapsed_ms=135",
    ]

    names = {1: "LEFT", 2: "UP", 3: "DOWN", 4: "RIGHT", 5: "OK"}
    for key in range(1, 6):
        name = names[key]
        lines.extend(
            [
                f"I KEYPAD: KEYPAD_RAW_TRANSITION raw={100 * key} from=NONE to={name}",
                f"I KEYPAD: KEYPAD_BUTTON_PRESS key={name} value={key}",
                f"I APP_EVENTS: KEYPAD_EVENT_POST key={key}",
                f"I UI_EVENT: KEYPAD_EVENT_DISPATCH key={key} age_ms={10 + key}",
                f"I UI_EVENT: UI_KEY_RESULT key={key} screen_before=0 screen_after={1 if key == 4 else 0} selected_before=0 selected_after={key % 5}",
                f"I KEYPAD: KEYPAD_RAW_TRANSITION raw=4095 from={name} to=NONE",
            ]
        )

    current = 23 if inherited else 1
    inherited_flag = 1 if inherited else 0
    lines.extend(
        [
            "I STORAGE: STORAGE_SPI_TIMING stage=begin mutex_wait_us=80 tft_quiesce_us=180 result=0",
            "I STORAGE: STORAGE_SPI_TIMING stage=end tft_resume_us=120 elapsed_us=900",
            "I REC: RECORDER_IO_TIMING op=fwrite requested=4096 written=4096 windows=2 max_window_us=450 fwrite_us=650 operation_us=1100 result=0",
            "I REC: STOP_COMPLETE stage=complete session=S0001 frames=1000 drops=0 err=ESP_OK",
            f"I STORAGE_MANAGER: STORAGE_PRIORITY stage=pre-release owner=web_status_cache priority_current={current} priority_base=1 inherited={inherited_flag} elapsed_us=900",
        ]
    )
    for channel in range(1, 5):
        lines.append(f"I AD7606: AD7606_DIAG: CH{channel} n=100 mean=0")
    lines.extend(
        [
            "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=STOP",
            "I WEB: WEB_STATUS_CACHE refresh=done",
        ]
    )
    return "\n".join(lines) + "\n"


def write_baseline(tmp_path: Path) -> Path:
    path = tmp_path / "baseline.json"
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "measured_at": "2026-09-13T17:00:00+03:00",
                "board": "ESP32-S3 N16R8",
                "sd_card": "64 GB A1 WICDXU1",
                "sdspi_hz": 20_000_000,
                "sample_rate_hz": 500,
                "channels": 4,
                "firmware_commit": "deadbeef",
                "limits_us": {
                    "mutex_wait_us": 100,
                    "tft_quiesce_us": 200,
                    "tft_resume_us": 150,
                    "spi_end_elapsed_us": 1000,
                    "recorder_window_us": 500,
                    "recorder_fwrite_us": 700,
                    "recorder_operation_us": 1200,
                },
                "notes": "synthetic unit-test profile only",
            }
        ),
        encoding="utf-8",
    )
    return path


def test_without_storage_baseline_only_max10_is_blocked():
    report = runner.analyze_report(healthy_log())

    assert report.machine_status == "BLOCKED"
    assert report.max6.machine_status == "PASS"
    assert report.max7.machine_status == "PASS"
    assert report.max10.machine_status == "BLOCKED"
    assert report.max11.machine_status == "PASS"
    assert report.storage_baseline_loaded is False


def test_valid_baseline_makes_all_machine_checks_pass(tmp_path: Path):
    report = runner.analyze_report(
        healthy_log(),
        storage_baseline_path=write_baseline(tmp_path),
    )

    assert report.machine_status == "PASS"
    assert report.max6.machine_status == "PASS"
    assert report.max7.machine_status == "PASS"
    assert report.max10.machine_status == "PASS"
    assert report.max11.machine_status == "PASS"
    assert report.storage_baseline_loaded is True
    assert report.max6.manual_required
    assert report.max7.manual_required


def test_require_inherited_blocks_base_only_capture(tmp_path: Path):
    report = runner.analyze_report(
        healthy_log(inherited=False),
        storage_baseline_path=write_baseline(tmp_path),
        require_inherited=True,
    )

    assert report.machine_status == "BLOCKED"
    assert report.max11.machine_status == "BLOCKED"
    assert any("inherited" in item for item in report.max11.evidence)


def test_targeted_inherited_sample_satisfies_reproduction_mode(tmp_path: Path):
    report = runner.analyze_report(
        healthy_log(inherited=True),
        storage_baseline_path=write_baseline(tmp_path),
        require_inherited=True,
    )

    assert report.machine_status == "PASS"
    assert report.max11.machine_status == "PASS"
    assert any("inherited samples=1" == item for item in report.max11.evidence)


def test_slow_startup_fails_max7_but_not_key_chain(tmp_path: Path):
    content = healthy_log().replace("startup_total=3200", "startup_total=6001")
    report = runner.analyze_report(
        content,
        storage_baseline_path=write_baseline(tmp_path),
    )

    assert report.machine_status == "FAIL"
    assert report.max6.machine_status == "PASS"
    assert report.max7.machine_status == "FAIL"


def test_missing_one_ui_result_blocks_max6(tmp_path: Path):
    content = healthy_log().replace(
        "I UI_EVENT: UI_KEY_RESULT key=5 screen_before=0 screen_after=0 selected_before=0 selected_after=0\n",
        "",
    )
    report = runner.analyze_report(
        content,
        storage_baseline_path=write_baseline(tmp_path),
    )

    assert report.machine_status == "BLOCKED"
    assert report.max6.machine_status == "BLOCKED"
    assert any("ui-result missing=5" in item for item in report.max6.evidence)


def test_invalid_baseline_is_report_configuration_error(tmp_path: Path):
    baseline = tmp_path / "bad.json"
    baseline.write_text('{"schema_version": 1}', encoding="utf-8")

    with pytest.raises(runner.AcceptanceError, match="invalid storage baseline"):
        runner.analyze_report(healthy_log(), storage_baseline_path=baseline)


def test_minimum_priority_samples_is_forwarded(tmp_path: Path):
    report = runner.analyze_report(
        healthy_log(),
        storage_baseline_path=write_baseline(tmp_path),
        minimum_priority_samples=2,
    )

    assert report.machine_status == "BLOCKED"
    assert report.max11.machine_status == "BLOCKED"
    assert any("(1/2)" in item for item in report.max11.evidence)
