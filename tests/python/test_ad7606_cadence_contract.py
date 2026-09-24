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


# MAX-247: the worker/stop/restart cadence rules used to be checked here by
# searching the source text (call order inside the worker, stop() clearing
# s_running before notifying, separate failure/collapse counters, status
# reset before task creation). They are now behavioural assertions in the
# host test components/ad7606_acquisition/tests/ad7606_acquisition_runtime_test.c,
# which runs the real worker and catches the same mutations (including the
# stop() ordering race, via a preempting xTaskNotifyGive). Only the
# cross-component wiring contract below remains a source contract.


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


def test_failed_ad7606_start_does_not_publish_zero_miss_proof():
    source = SOURCE_AD7606.read_text(encoding="utf-8")
    assert "snapshot->cadence_available = true;" not in source

    availability = _between(
        source,
        "snapshot->cadence_available =",
        "snapshot->timer_ticks =",
    )
    assert "status.running" in availability
    assert "status.timer_ticks > 0U" in availability


def test_session_boundary_uses_delta_and_legacy_metadata_remains_unknown():
    recorder = RECORDER.read_text(encoding="utf-8")
    metadata = RECORDER_JSON.read_text(encoding="utf-8")

    assert "s_cadence_start = cadence_start" in recorder
    assert "seismic_recorder_cadence_delta(" in recorder
    assert 'cJSON_GetObjectItemCaseSensitive(root, "cadence")' in metadata
    assert "Schema v1 predates cadence evidence" in metadata


def test_saved_api_exposes_bucket_end_timestamp():
    text = WEB_SEISMIC.read_text(encoding="utf-8")
    assert '\\"end_timestamp_us\\"' in text
    assert "points[point_index].end_timestamp_us" in text
