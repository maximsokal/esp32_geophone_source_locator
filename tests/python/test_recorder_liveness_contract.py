"""Static contract for recorder liveness placement around blocking operations."""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RECORDER = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder.c"
RECORDER_INTERNAL = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder_internal.h"
RECORDER_FS = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder_fs.c"
RECORDER_HEADER = REPO_ROOT / "components" / "seismic_recorder" / "include" / "seismic_recorder.h"
MEASUREMENT_SERVICE = REPO_ROOT / "components" / "measurement_service" / "measurement_service.c"
WEBUI_CONSOLE = REPO_ROOT / "webui" / "src" / "DeviceConsoleApp.tsx"
WEBUI_MEASUREMENT_TRANSITION = REPO_ROOT / "webui" / "src" / "features" / "objects" / "measurementTransition.ts"
WEB_STATUS_JSON = REPO_ROOT / "components" / "web_server" / "web_status_json.c"
WEB_STATUS_RECORDING = REPO_ROOT / "components" / "web_server" / "web_status_json_recording.c"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def _define_int(source: str, name: str, suffix: str) -> int:
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(\d+){re.escape(suffix)}\s*$",
        source,
        re.MULTILINE,
    )
    assert match is not None, f"missing numeric define: {name}"
    return int(match.group(1))


def test_recorder_batch_is_psram_backed_and_amortizes_sd_operations():
    source = RECORDER.read_text(encoding="utf-8")
    internal = RECORDER_INTERNAL.read_text(encoding="utf-8")
    filesystem = RECORDER_FS.read_text(encoding="utf-8")
    init = _function_body(source, "esp_err_t seismic_recorder_init(void)")

    # A 500 Hz stream produces 250 frames in the normal write window. Keep a
    # bounded headroom batch for a delayed SD operation without putting the
    # non-DMA copy buffer in scarce internal/DMA RAM.
    assert _define_int(source, "SEISMIC_RECORDER_WRITE_PERIOD_MS", "U") == 500
    assert _define_int(internal, "SEISMIC_RECORDER_BATCH_FRAMES", "U") == 512
    assert _define_int(internal, "SEISMIC_RECORDER_WRITE_STAGING_FRAMES", "U") == 256
    assert _define_int(internal, "SEISMIC_RECORDER_FLUSH_INTERVAL_US", "LL") == 30_000_000
    assert "static seismic_frame_t *s_batch = NULL;" in source
    assert "s_batch = heap_caps_malloc(" in init
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in init
    assert "heap_caps_free(s_batch);" in init

    # ESP-IDF 5.5.4's SDSPI driver bounces external-RAM sources one sector at
    # a time. The recorder therefore presents fwrite with a bounded internal
    # DMA_ATTR buffer while retaining the larger PSRAM backlog copy.
    assert "DMA_ATTR static seismic_frame_t s_dma_staging" in filesystem
    assert "memcpy(" in filesystem and "s_dma_staging" in filesystem
    assert "s_dma_staging," in filesystem
    assert "fwrite(\n            s_dma_staging," in filesystem
    assert "fwrite(\n        frames," not in filesystem

    # Periodic flush is deliberately sparse; durability remains explicit in
    # the STOP state machine rather than being paid on every small batch.
    stop_step = _function_body(source, "static void seismic_recorder_stop_step")
    assert "seismic_recorder_fs_flush(file, false)" in stop_step
    assert "seismic_recorder_fs_sync(file)" in stop_step


def test_recorder_heartbeat_is_post_operation_and_progress_is_snapshot_based():
    source = RECORDER.read_text(encoding="utf-8")
    task = _function_body(source, "static void seismic_recorder_task")
    observe = _function_body(source, "static void seismic_recorder_observe")

    # The task loop must use the post-operation helper instead of publishing
    # a heartbeat at loop entry, before copy/write/flush can block.
    assert "runtime_observability_beat(" in observe
    assert "runtime_observability_beat(" not in task
    assert task.count("seismic_recorder_observe();") == 9

    copy = task.index("seismic_buffer_copy_since(")
    write = task.index("seismic_recorder_fs_write_frames(")
    flush = task.index("seismic_recorder_flush_if_due(file)")
    observations = [
        index
        for index in range(len(task))
        if task.startswith("seismic_recorder_observe();", index)
    ]
    next_observe = lambda operation: next(index for index in observations if index > operation)
    assert next_observe(copy) > copy
    assert next_observe(write) > write
    assert next_observe(flush) > flush

    # The helper reads the committed recorder counter after taking and
    # releasing the recorder mutex, then publishes it. A stale local counter
    # cannot make a blocked SD operation look like progress.
    assert observe.index("seismic_recorder_lock();") < observe.index("samples_written = s_snapshot.samples_written;")
    assert observe.index("samples_written = s_snapshot.samples_written;") < observe.index("seismic_recorder_unlock();")
    assert observe.index("seismic_recorder_unlock();") < observe.index("runtime_observability_beat(")


def test_stop_is_async_and_wait_helper_has_no_soft_timeout():
    source = RECORDER.read_text(encoding="utf-8")
    stop = _function_body(source, "esp_err_t seismic_recorder_stop")
    wait = _function_body(source, "esp_err_t seismic_recorder_wait_for_stop")
    task = _function_body(source, "static void seismic_recorder_task")
    step = _function_body(source, "static void seismic_recorder_stop_step")
    header = RECORDER_HEADER.read_text(encoding="utf-8")

    assert "SEISMIC_RECORDER_STOP_TIMEOUT_MS" not in source
    assert "SEISMIC_RECORDER_STOP_STALL_TIMEOUT_MS" not in source
    assert "STOP_STALL watchdog" not in source
    assert "xEventGroupWaitBits(" not in stop
    assert "STOP_REQUEST" in stop
    assert "portMAX_DELAY" in wait
    assert "seismic_recorder_wait_for_stop" in header
    assert "seismic_recorder_stop_step();" in task
    assert "SEISMIC_RECORDER_STOP_STAGE_DRAIN" in task
    assert "SEISMIC_RECORDER_STOP_STAGE_FLUSH" in task
    assert "SEISMIC_RECORDER_STOP_STAGE_SYNC" in step
    assert "SEISMIC_RECORDER_STOP_STAGE_CLOSE" in step
    assert "SEISMIC_RECORDER_STOP_STAGE_METADATA" in step


def test_stop_stage_error_is_carried_to_completion_status():
    source = RECORDER.read_text(encoding="utf-8")
    step = _function_body(source, "static void seismic_recorder_stop_step")

    assert "seismic_recorder_record_stop_error(" in step
    assert "stop_error_stage" in step
    assert "stop_stage_error" in step
    assert "STOP_COMPLETE stage=" in step


def test_measurement_service_polls_until_recorder_really_finishes():
    source = MEASUREMENT_SERVICE.read_text(encoding="utf-8")
    stop = _function_body(source, "static void measurement_service_process_stop(void)")
    poll = _function_body(source, "static void measurement_service_process_stop_poll(void)")

    assert "measurement_service_process_stop_poll();" in stop
    assert "recorder.state == SEISMIC_RECORDER_STOPPING" in poll
    assert "s_snapshot.state = MEASUREMENT_SERVICE_REVIEW;" in poll


def test_status_api_exposes_measurement_service_terminal_states():
    status_source = WEB_STATUS_JSON.read_text(encoding="utf-8")
    recording_source = WEB_STATUS_RECORDING.read_text(encoding="utf-8")

    assert "web_status_json_add_measurement(root)" in status_source
    assert "measurement_service_get_snapshot" in recording_source
    assert '"state",' in recording_source
    assert 'return "RECORDING";' in recording_source
    assert 'return "REVIEW";' in recording_source
    assert 'return "ERROR";' in recording_source


def test_measurement_snapshot_captures_state_and_busy_under_one_lock():
    source = MEASUREMENT_SERVICE.read_text(encoding="utf-8")
    snapshot = _function_body(
        source,
        "esp_err_t measurement_service_get_snapshot("
    )

    lock = snapshot.index("measurement_service_lock();")
    state_copy = snapshot.index("*snapshot = s_snapshot;")
    busy = snapshot.index("snapshot->busy = measurement_service_is_busy();")
    unlock = snapshot.index("measurement_service_unlock();")

    assert lock < state_copy < busy < unlock


def test_console_record_button_uses_the_async_measurement_state_machine():
    source = WEBUI_CONSOLE.read_text(encoding="utf-8")
    transition_source = WEBUI_MEASUREMENT_TRANSITION.read_text(encoding="utf-8")
    start = source.index("onRecording={async")
    end = source.index("onOpenScanner=", start)
    recording_control = source[start:end]

    assert "running ? 'measurement_start' : 'measurement_stop'" in recording_control
    assert "recording_start" not in recording_control
    assert "recording_stop" not in recording_control
    assert "const MEASUREMENT_TRANSITION_ACTIONS = new Set(['measurement_start', 'measurement_stop']);" in source
    assert "measurementTransitionNeedsSessionRefresh" in source
    assert "waitForStorageCacheIdle" in source

    transition = source.index("waitForMeasurementTransition(")
    refresh = source.index("await refreshSessions(confirmedObjectId)")
    assert transition < refresh
    refresh_gate = source.index("if (measurementTransitionNeedsSessionRefresh(", transition)
    assert refresh_gate < refresh
    cache_idle = source.index("await waitForStorageCacheIdle(", refresh_gate)
    assert refresh_gate < cache_idle < refresh
    assert "const confirmedObjectId = selectedId" in source[refresh_gate:refresh]
    assert "measurementTransitionErrorMessage(transitionStatus)" in source
    assert "status.measurement?.busy !== true" in transition_source
    assert "status.storage_busy !== true" in transition_source
