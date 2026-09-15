from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ACQUISITION = REPO_ROOT / "components" / "ad7606_acquisition" / "ad7606_acquisition.c"
SOURCE_AD7606 = REPO_ROOT / "components" / "seismic_acquisition" / "seismic_source_ad7606.c"
WEB_STATUS = REPO_ROOT / "components" / "web_server" / "web_status_json.c"
RECORDER = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder.c"
RECORDER_JSON = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder_json.c"
WEB_SEISMIC = REPO_ROOT / "components" / "web_server" / "web_seismic_api.c"


def _between(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    finish = text.index(end, begin)
    return text[begin:finish]


def test_worker_accounts_only_running_timer_wakes_and_never_burst_catches_up():
    text = ACQUISITION.read_text(encoding="utf-8")
    worker = _between(text, "static void ad7606_worker", "esp_err_t ad7606_acquisition_start")

    notify = worker.index("ulTaskNotifyTake")
    stop_guard = worker.index("if (!s_running)", notify)
    account = worker.index("ad7606_status_record_timer_wake", stop_guard)
    physical_read = worker.index("ad7606_driver_read", account)
    failure = worker.index("ad7606_status_record_failure", physical_read)

    assert notify < stop_guard < account < physical_read < failure
    assert worker.count("ad7606_driver_read(") == 1


def test_stop_notification_is_marked_manual_before_worker_is_woken():
    text = ACQUISITION.read_text(encoding="utf-8")
    stop = text[text.index("void ad7606_acquisition_stop") :]
    assert stop.index("s_running =") < stop.index("xTaskNotifyGive")


def test_read_failure_counter_is_distinct_from_scheduler_collapse_counter():
    text = ACQUISITION.read_text(encoding="utf-8")
    failure = _between(
        text,
        "static uint32_t ad7606_status_record_failure",
        "static void ad7606_status_record_sample",
    )
    timer = _between(
        text,
        "static void ad7606_status_record_timer_wake",
        "static uint32_t ad7606_status_record_failure",
    )

    assert "read_errors" in failure
    assert "consecutive_failures" in failure
    assert "collapsed_periods" not in failure
    assert "read_errors" not in timer
    assert "conversion_attempts" in timer
    assert "collapsed_periods" in timer


def test_acquisition_restart_resets_cadence_state_before_worker_creation():
    text = ACQUISITION.read_text(encoding="utf-8")
    start = _between(text, "esp_err_t ad7606_acquisition_start", "void ad7606_acquisition_stop")
    assert start.index("ad7606_status_reset()") < start.index("xTaskCreatePinnedToCore")


def test_generic_snapshot_and_status_expose_cadence_without_private_web_dependency():
    source = SOURCE_AD7606.read_text(encoding="utf-8")
    status = WEB_STATUS.read_text(encoding="utf-8")

    for field in (
        "cadence_available",
        "timer_ticks",
        "conversion_attempts",
        "collapsed_periods",
        "last_successful_sample_interval_us",
        "max_successful_sample_interval_us",
    ):
        assert f"snapshot->{field}" in source
        assert f'"{field}"' in status or field == "cadence_available"

    assert 'cJSON_AddObjectToObject(object, "cadence")' in status
    assert "source_snapshot->cadence_available" in status


def test_session_boundary_uses_delta_and_legacy_metadata_remains_unknown():
    recorder = RECORDER.read_text(encoding="utf-8")
    metadata = RECORDER_JSON.read_text(encoding="utf-8")

    assert "s_cadence_start = cadence_start" in recorder
    assert "seismic_recorder_cadence_delta(" in recorder
    assert 'cJSON_GetObjectItemCaseSensitive(root, "cadence")' in metadata
    assert "Schema v1 predates cadence evidence" in metadata


def test_saved_api_exposes_bucket_end_timestamp():
    text = WEB_SEISMIC.read_text(encoding="utf-8")
    assert '"end_timestamp_us"' in text
    assert "points[point_index].end_timestamp_us" in text
