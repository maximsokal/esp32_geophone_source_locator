"""Static contract for recorder liveness placement around blocking operations."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RECORDER = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder.c"
RECORDER_HEADER = REPO_ROOT / "components" / "seismic_recorder" / "include" / "seismic_recorder.h"
MEASUREMENT_SERVICE = REPO_ROOT / "components" / "measurement_service" / "measurement_service.c"


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
