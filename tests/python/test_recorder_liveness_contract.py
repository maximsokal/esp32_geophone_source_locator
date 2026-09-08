"""Static contract for recorder liveness placement around blocking operations."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RECORDER = REPO_ROOT / "components" / "seismic_recorder" / "seismic_recorder.c"


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
    assert task.count("seismic_recorder_observe();") == 8

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
