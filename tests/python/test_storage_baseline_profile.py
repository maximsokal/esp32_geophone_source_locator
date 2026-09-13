from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
WRAPPER_PATH = REPO_ROOT / "tools" / "analyze_storage_baseline.py"

_spec = importlib.util.spec_from_file_location("analyze_storage_baseline", WRAPPER_PATH)
assert _spec is not None and _spec.loader is not None
wrapper = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = wrapper
_spec.loader.exec_module(wrapper)


VALID = {
    "schema_version": 1,
    "measured_at": "2026-09-13T16:30:00+03:00",
    "board": "ESP32-S3 N16R8",
    "sd_card": "64 GB A1 WICDXU1",
    "sdspi_hz": 20_000_000,
    "sample_rate_hz": 500,
    "channels": 4,
    "firmware_commit": "0123456789abcdef",
    "limits_us": {
        "mutex_wait_us": 100,
        "tft_quiesce_us": 200,
        "tft_resume_us": 150,
        "spi_end_elapsed_us": 1000,
        "recorder_window_us": 500,
        "recorder_fwrite_us": 700,
        "recorder_operation_us": 1200,
    },
    "notes": "measured baseline",
}


def write_json(tmp_path: Path, payload: object) -> Path:
    path = tmp_path / "baseline.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def test_valid_profile_loads_all_metadata_and_limits(tmp_path: Path):
    baseline = wrapper.load_baseline(write_json(tmp_path, VALID))

    assert baseline.measured_at == VALID["measured_at"]
    assert baseline.board == VALID["board"]
    assert baseline.sd_card == VALID["sd_card"]
    assert baseline.sdspi_hz == 20_000_000
    assert baseline.sample_rate_hz == 500
    assert baseline.channels == 4
    assert baseline.firmware_commit == VALID["firmware_commit"]
    assert baseline.limits_us == VALID["limits_us"]


def test_template_style_null_limits_are_rejected(tmp_path: Path):
    payload = dict(VALID)
    payload["limits_us"] = dict(VALID["limits_us"])
    payload["limits_us"]["mutex_wait_us"] = None

    with pytest.raises(wrapper.BaselineError, match="mutex_wait_us"):
        wrapper.load_baseline(write_json(tmp_path, payload))


def test_missing_measured_at_or_commit_is_rejected(tmp_path: Path):
    for key in ("measured_at", "firmware_commit"):
        payload = dict(VALID)
        payload[key] = None
        with pytest.raises(wrapper.BaselineError, match=key):
            wrapper.load_baseline(write_json(tmp_path, payload))


def test_schema_version_is_strict(tmp_path: Path):
    payload = dict(VALID)
    payload["schema_version"] = 2

    with pytest.raises(wrapper.BaselineError, match="schema_version must be 1"):
        wrapper.load_baseline(write_json(tmp_path, payload))


def test_unknown_limit_key_is_rejected(tmp_path: Path):
    payload = dict(VALID)
    payload["limits_us"] = dict(VALID["limits_us"])
    payload["limits_us"]["invented_latency_us"] = 123

    with pytest.raises(wrapper.BaselineError, match="unknown keys"):
        wrapper.load_baseline(write_json(tmp_path, payload))


def test_limit_range_is_checked(tmp_path: Path):
    for bad in (-1, 60_000_001):
        payload = dict(VALID)
        payload["limits_us"] = dict(VALID["limits_us"])
        payload["limits_us"]["recorder_operation_us"] = bad
        with pytest.raises(wrapper.BaselineError, match="0..60000000"):
            wrapper.load_baseline(write_json(tmp_path, payload))


def test_positive_numeric_metadata_is_required(tmp_path: Path):
    for key in ("sdspi_hz", "sample_rate_hz", "channels"):
        payload = dict(VALID)
        payload[key] = 0
        with pytest.raises(wrapper.BaselineError, match=key):
            wrapper.load_baseline(write_json(tmp_path, payload))


def test_channels_are_limited_to_ad7606_width(tmp_path: Path):
    payload = dict(VALID)
    payload["channels"] = 9

    with pytest.raises(wrapper.BaselineError, match="1..8"):
        wrapper.load_baseline(write_json(tmp_path, payload))


def test_boolean_is_not_accepted_as_integer(tmp_path: Path):
    payload = dict(VALID)
    payload["limits_us"] = dict(VALID["limits_us"])
    payload["limits_us"]["mutex_wait_us"] = True

    with pytest.raises(wrapper.BaselineError, match="must be an integer"):
        wrapper.load_baseline(write_json(tmp_path, payload))
