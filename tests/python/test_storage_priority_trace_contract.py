"""Source contracts for targeted storage-owner priority evidence.

These contracts prove placement and non-mutating intent of the diagnostic
marker. They do not prove the observed priority values on hardware.
"""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = (
    REPO_ROOT / "components" / "storage_manager" / "storage_manager.c"
).read_text(encoding="utf-8")


def section(source: str, start_marker: str, end_marker: str | None = None) -> str:
    start = source.find(start_marker)
    assert start >= 0, f"missing source marker: {start_marker}"
    if end_marker is None:
        return source[start:]
    end = source.find(end_marker, start + len(start_marker))
    assert end >= 0, f"missing source marker: {end_marker}"
    return source[start:end]


def test_priority_trace_reads_current_task_without_changing_priority():
    helper = section(
        SOURCE,
        "static void storage_manager_log_owner_priority_before_release(",
        "void storage_manager_operation_end(void)",
    )

    assert "vTaskGetInfo(" in helper
    assert "NULL," in helper
    assert "status.uxCurrentPriority" in helper
    assert "status.uxBasePriority" in helper
    assert "status.uxCurrentPriority > status.uxBasePriority" in helper
    assert "STORAGE_PRIORITY stage=pre-release" in helper

    # Diagnostic code must remain observational only.
    assert "vTaskPrioritySet" not in helper
    assert "uxTaskPriorityGet" not in helper
    assert "xSemaphoreTake" not in helper
    assert "xSemaphoreGive" not in helper


def test_web_status_cache_is_sampled_even_without_inherited_boost():
    helper = section(
        SOURCE,
        "static bool storage_manager_is_web_status_cache_task(",
        "void storage_manager_operation_end(void)",
    )

    # ESP-IDF may truncate the configured task name to `web_status_cach`.
    assert '"web_status_cach"' in helper
    assert "if (!inherited && !web_status_cache)" in helper


def test_targeted_sample_occurs_before_recursive_mutex_release():
    operation_end = section(SOURCE, "void storage_manager_operation_end(void)")

    outermost = operation_end.find("if (outermost)")
    trace = operation_end.find("storage_manager_log_owner_priority_before_release(")
    give = operation_end.find("xSemaphoreGiveRecursive(", trace)
    timing_log = operation_end.find('"STORAGE_OP_TIMING stage=end', give)

    assert outermost >= 0
    assert trace > outermost
    assert give > trace
    assert timing_log > give


def test_trace_uses_existing_trace_facility_contract():
    helper = section(
        SOURCE,
        "static void storage_manager_log_owner_priority_before_release(",
        "void storage_manager_operation_end(void)",
    )

    assert "#if configUSE_TRACE_FACILITY == 1" in helper
    assert "#else" in helper
    assert "(void)elapsed_us;" in helper
    assert "#endif" in helper
