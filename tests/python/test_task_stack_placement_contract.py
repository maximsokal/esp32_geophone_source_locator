"""Task stacks that must live in PSRAM.

Board log 2026-11-13: internal RAM is down to ~5 KB once Wi-Fi, the web server and the UI are up (100 KB after LVGL,
26 KB after Wi-Fi). Every task below is created with its stack in PSRAM; one that falls back to an internal stack
fails to start (or starves the SPI/SD DMA reserve). The host tests build the non-ESP branch, so this pins the ESP one.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# file -> (a task creation call that must carry PSRAM caps).
# NOT here on purpose: the AD7606 worker, which asks for an internal stack first and only falls back to PSRAM.
# The recorder IS here: its 4 KB internal stack overflowed in the ESP-EMU recording scenario (S10).
PSRAM_STACK_TASKS = {
    "components/seismic_recorder/seismic_recorder.c": "seismic_recorder_task_create(",
    "components/ina3221_monitor/ina3221_monitor.c": "ina3221_monitor_task_create(",
    "components/ui/ui_workflow.c": "ui_workflow_task_create(",
    "components/ui/ui_scan_preview.c": "ui_scan_preview_task_create(",
}

# Already moved before; direct calls.
DIRECT_WITH_CAPS = (
    "components/nrf_topology_service/nrf_topology_service.c",
    "components/nrf_emergency_stop_monitor/nrf_emergency_stop_monitor.c",
    "components/device_status_pod_bridge/app_pod_status_bridge.c",
    "components/storage_service/storage_service.c",
    "components/object_service/object_service.c",
    "components/measurement_service/measurement_service.c",
)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_helper_tasks_use_psram_stack_on_esp_and_plain_create_only_on_host():
    for path, call in PSRAM_STACK_TASKS.items():
        source = read(path)
        helper = call[:-1]
        assert source.count(call) == 2, f"{path}: one definition and one use of {helper}"
        match = re.search(
            r"#if defined\(ESP_PLATFORM\)\s*"
            r"return xTaskCreatePinnedToCoreWithCaps\([^;]*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\);\s*"
            r"#else\s*return xTaskCreatePinnedToCore\([^;]*\);\s*#endif",
            source,
        )
        assert match, f"{path}: ESP branch must create the task with a PSRAM stack"
        assert source.count("xTaskCreatePinnedToCore(") == 1, f"{path}: only the host branch may use the plain call"


def test_ina3221_worker_deletes_itself_with_caps_on_esp():
    source = read("components/ina3221_monitor/ina3221_monitor.c")
    assert re.search(r"#if defined\(ESP_PLATFORM\)\s*vTaskDeleteWithCaps\(NULL\);\s*#else\s*vTaskDelete\(NULL\);", source)
    assert source.count("vTaskDelete(") == 1, "only the host branch may use the plain delete"
    assert "ina3221_monitor_task_delete_self();" in source


def test_other_service_tasks_keep_psram_stacks():
    for path in DIRECT_WITH_CAPS:
        source = read(path)
        assert "xTaskCreatePinnedToCoreWithCaps(" in source, path
        assert "MALLOC_CAP_SPIRAM" in source, path
        assert "xTaskCreatePinnedToCore(" not in source, f"{path}: no task may fall back to an internal stack"
