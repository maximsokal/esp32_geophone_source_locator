"""MAX-480 host-test wiring and bounded-stop contracts."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LIMITS = ROOT / "components" / "app_config" / "include" / "app_stop_timeout_config.h"
LOCALIZATION = ROOT / "components" / "localization_job" / "localization_job.c"
LOCALIZATION_TEST = ROOT / "components" / "localization_job" / "tests" / "localization_job_init_fault_test.c"
WORKFLOW = ROOT / "components" / "workflow_service" / "workflow_service.c"
WORKFLOW_HEADER = ROOT / "components" / "workflow_service" / "include" / "workflow_service.h"
WORKFLOW_TEST = ROOT / "tests" / "host" / "workflow_service_test.c"
MEASUREMENT = ROOT / "components" / "measurement_service" / "measurement_service.c"
MEASUREMENT_HEADER = ROOT / "components" / "measurement_service" / "include" / "measurement_service.h"
MEASUREMENT_TEST = ROOT / "components" / "measurement_service" / "tests" / "measurement_service_worker_contract_test.c"
RECORDER = ROOT / "components" / "seismic_recorder" / "seismic_recorder.c"
RECORDER_TEST = ROOT / "components" / "seismic_recorder" / "tests" / "seismic_recorder_allocation_contract_test.c"
SD_FORMAT = ROOT / "components" / "sd_card" / "sd_card_format.c"
SD_FORMAT_TEST = ROOT / "components" / "sd_card" / "tests" / "sd_card_format_host_test.c"
LOCALIZATION_MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "max480-lifecycle.json"
BOUNDED_MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "max481-bounded-queues.json"
SD_MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "coverage-sd-card-recovery.json"
ROOT_MANIFEST = ROOT / "tests" / "host" / "manifest.json"


def _body(source: str, signature: str) -> str:
    match = re.search(re.escape(signature) + r"\s*\{", source)
    assert match is not None, f"missing function definition: {signature}"
    opening = match.end() - 1
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def _positive_define(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)U\s*$", source, re.MULTILINE)
    assert match is not None, f"missing stop-limit constant {name}"
    value = int(match.group(1))
    assert value > 0, f"{name} must be positive"
    return value


def _manifest_target(path: Path, name: str) -> dict:
    entries = [entry for entry in json.loads(path.read_text(encoding="utf-8"))["tests"] if entry["name"] == name]
    assert len(entries) == 1, f"expected one {name} target in {path.relative_to(ROOT)}"
    return entries[0]


def test_stop_limits_are_positive_provisional_and_used():
    limits = LIMITS.read_text(encoding="utf-8")
    assert "provisional" in limits.lower() and "worst-case i/o measurements" in limits.lower()
    _positive_define(limits, "APP_STOP_TIMEOUT_WORKER_MS")
    _positive_define(limits, "APP_STOP_TIMEOUT_RECORDING_MS")
    _positive_define(limits, "APP_STOP_TIMEOUT_FORMAT_MS")


def test_localization_failure_cleanup_test_is_reachable_and_registered():
    source = LOCALIZATION.read_text(encoding="utf-8")
    init = _body(source, "esp_err_t localization_job_init(void)")
    assert "if (s_mutex == NULL || s_wake == NULL)" in init
    partial = init.split("if (s_mutex == NULL || s_wake == NULL)", 1)[1].split("/*", 1)[0]
    assert "vSemaphoreDelete(s_wake)" in partial
    assert "vSemaphoreDelete(s_mutex)" in partial
    assert "s_wake = NULL" in partial and "s_mutex = NULL" in partial
    failed_task = init.split("if (created != pdPASS)", 1)[1]
    assert "vSemaphoreDelete(s_wake)" in failed_task
    assert "vSemaphoreDelete(s_mutex)" in failed_task

    test = LOCALIZATION_TEST.read_text(encoding="utf-8")
    assert "test_partial_allocation_and_task_create_failures_can_retry();" in _body(test, "int main(void)")
    entry = _manifest_target(LOCALIZATION_MANIFEST, "max480_localization_job_init")
    assert "components/localization_job/tests/localization_job_init_fault_test.c" in entry["sources"]
    root_entry = _manifest_target(ROOT_MANIFEST, "max480_localization_job_init")
    assert root_entry["sources"] == entry["sources"]


def test_workflow_stop_is_cooperative_acknowledged_and_wired():
    source = WORKFLOW.read_text(encoding="utf-8")
    shutdown = _body(source, "esp_err_t workflow_service_shutdown(void)")
    assert "vTaskDelete(s_poll_task)" not in shutdown
    assert "atomic_store_explicit(&s_poll_stop_requested, true" in shutdown
    assert "xSemaphoreTake(" in shutdown
    assert "pdMS_TO_TICKS(APP_STOP_TIMEOUT_WORKER_MS)" in shutdown
    assert "return ESP_ERR_TIMEOUT;" in shutdown

    worker = _body(source, "static void workflow_service_poll_task(void *argument)")
    assert worker.index("s_poll_task = NULL;") < worker.index("xSemaphoreGive(s_poll_stop_ack);")
    assert "atomic_load_explicit(&s_poll_stop_requested" in worker
    header = WORKFLOW_HEADER.read_text(encoding="utf-8")
    assert "esp_err_t workflow_service_shutdown(void);" in header

    test = WORKFLOW_TEST.read_text(encoding="utf-8")
    assert "test_poll_shutdown_ack_precedes_resource_destroy();" in _body(test, "int main(void)")
    entry = _manifest_target(BOUNDED_MANIFEST, "max481_workflow_service")
    assert "tests/host/stubs/workflow_service/host_rtos.c" in entry["sources"]
    assert "tests/host/workflow_service_test.c" in entry["sources"]


def test_measurement_shutdown_times_out_without_releasing_live_resources():
    source = MEASUREMENT.read_text(encoding="utf-8")
    shutdown = _body(source, "esp_err_t measurement_service_shutdown(void)")
    assert "portMAX_DELAY" not in shutdown
    assert "pdMS_TO_TICKS(APP_STOP_TIMEOUT_WORKER_MS)" in shutdown
    assert "return ESP_ERR_TIMEOUT;" in shutdown
    assert shutdown.index("return ESP_ERR_TIMEOUT;") < shutdown.index("vSemaphoreDelete(s_mutex)")
    assert "esp_err_t measurement_service_shutdown(void);" in MEASUREMENT_HEADER.read_text(encoding="utf-8")

    test = MEASUREMENT_TEST.read_text(encoding="utf-8")
    assert "test_shutdown_timeout_retains_worker_owned_resources();" in _body(test, "int main(void)")
    entry = _manifest_target(BOUNDED_MANIFEST, "max481_measurement_service")
    assert "components/measurement_service/tests/measurement_service_worker_contract_test.c" in entry["sources"]


def test_recorder_wait_is_bounded_and_marks_timeout_incomplete():
    source = RECORDER.read_text(encoding="utf-8")
    wait = _body(source, "esp_err_t seismic_recorder_wait_for_stop(void)")
    assert "portMAX_DELAY" not in wait
    assert "pdMS_TO_TICKS(APP_STOP_TIMEOUT_RECORDING_MS)" in wait
    assert "seismic_recorder_record_stop_error(" in wait
    assert "return ESP_ERR_TIMEOUT;" in wait
    test = RECORDER_TEST.read_text(encoding="utf-8")
    assert "test_stop_timeout_marks_recording_incomplete();" in _body(test, "int main(void)")
    entry = _manifest_target(ROOT_MANIFEST, "seismic_recorder_allocation_contract")
    assert "components/seismic_recorder/tests/seismic_recorder_allocation_contract_test.c" in entry["sources"]


def test_sd_format_stop_joins_and_preserves_state_on_timeout():
    source = SD_FORMAT.read_text(encoding="utf-8")
    stop = _body(source, "esp_err_t sd_card_format_fat32_task_stop(void)")
    assert "pdMS_TO_TICKS(APP_STOP_TIMEOUT_FORMAT_MS)" in stop
    assert "vTaskDelay(1U)" in stop
    assert "s_stop_timed_out = true;" in stop
    assert "return ESP_ERR_TIMEOUT;" in stop
    test = SD_FORMAT_TEST.read_text(encoding="utf-8")
    main = _body(test, "int main(void)")
    assert "test_stop_joins_cooperative_worker_before_return();" in main
    assert "test_stop_timeout_marks_incomplete_and_retains_worker();" in main
    entry = _manifest_target(SD_MANIFEST, "sd_card_format_flow")
    assert "components/sd_card/tests/sd_card_format_host_test.c" in entry["sources"]
