"""The HTTP task has an 8 KB stack, and a request for the workflow walks down to the SPI driver.

Board log 2026-11-16 20:32: "start measurement" from the browser crashed twice in a row. The first time the heap
allocator asserted (block_trim_free: a neighbouring block had been overwritten), the second time printf faulted
inside storage_manager_operation_begin. Both frames sat in the HTTP task, whose call chain is

    web_api_control_handler -> web_status_json_add_workflow (2.6 KB frame, a workflow snapshot)
      -> workflow_service_get_snapshot -> presence refresh -> object_store_scan_present -> storage_manager_path_exists
      -> stat -> FAT -> SDSPI -> SPI driver (malloc of the DMA buffer) / ESP_LOG

The shared-scan lookup added to that chain pushed it past the stack. A snapshot is about 2.5 KB; on this path it
belongs on the heap (malloc puts a block that size in PSRAM). These tests pin that, since a host test cannot measure a
stack.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_no_workflow_snapshot_lives_on_the_http_task_stack() -> None:
    for relative in (
        "components/web_server/web_status_json_recording.c",
        "components/web_server/web_api_control.c",
    ):
        source = _read(relative)
        # "workflow_snapshot_t name;" or "workflow_snapshot_t name = ..." as a local (no '*').
        stack_locals = re.findall(r"^\s+workflow_snapshot_t\s+\w+\s*(?:;|=)", source, re.MULTILINE)
        assert stack_locals == [], f"{relative} keeps a workflow snapshot on the stack: {stack_locals}"


def test_workflow_snapshot_is_allocated_and_freed_where_the_deep_call_is_made() -> None:
    status = _read("components/web_server/web_status_json_recording.c")
    public = _function_body(status, "bool web_status_json_add_workflow(cJSON *root)")
    assert "malloc(sizeof(*snapshot))" in public
    assert public.index("malloc(") < public.index("workflow_service_get_snapshot(snapshot)")
    assert public.index("workflow_service_get_snapshot(snapshot)") < public.index("free(snapshot)")
    fill = _function_body(status, "static bool web_status_json_add_workflow_fill(")
    assert "workflow_service_get_snapshot" not in fill

    control = _read("components/web_server/web_api_control.c")
    start = _function_body(control, "static esp_err_t web_control_measurement_start(")
    assert "malloc(sizeof(*workflow))" in start
    # Released before the next deep call (seismic_acquisition_start / measurement_service_start_object_async).
    assert start.index("free(workflow)") < start.index("seismic_acquisition_start()")
    assert "workflow->step" in start
    # Nothing reads the snapshot after it is freed.
    assert "workflow->" not in start[start.index("free(workflow)") :]
