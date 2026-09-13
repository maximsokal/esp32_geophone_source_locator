"""Architecture contracts for fast interactive startup and cooperative recovery.

These are source/ordering contracts, not physical startup-time proof. The
<=5 s acceptance target still requires a live board log with the real SD card.
"""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
STARTUP = (REPO_ROOT / "main" / "app_startup.c").read_text(encoding="utf-8")
SHUTDOWN = (REPO_ROOT / "main" / "app_shutdown.c").read_text(encoding="utf-8")
RECOVERY = (
    REPO_ROOT / "components" / "object_recovery" / "object_recovery.c"
).read_text(encoding="utf-8")
WORKER = (
    REPO_ROOT / "components" / "object_recovery" / "object_recovery_worker.c"
).read_text(encoding="utf-8")
TASK_CONFIG = (
    REPO_ROOT / "components" / "app_config" / "include" / "app_task_config.h"
).read_text(encoding="utf-8")
WEB_PREPARE = (
    REPO_ROOT / "components" / "web_server" / "web_server_prepare.c"
).read_text(encoding="utf-8")
WEB_ARBITRATION = (
    REPO_ROOT / "components" / "web_server" / "web_status_cache_arbitration.c"
).read_text(encoding="utf-8")
APP_EVENTS = (
    REPO_ROOT / "components" / "app_events" / "include" / "app_events.h"
).read_text(encoding="utf-8")
UI_NAVIGATION = (
    REPO_ROOT / "components" / "ui" / "ui_navigation.c"
).read_text(encoding="utf-8")
UI_HOME = (
    REPO_ROOT / "components" / "ui" / "ui_home.c"
).read_text(encoding="utf-8")


def position(source: str, needle: str) -> int:
    value = source.find(needle)
    assert value >= 0, f"missing source contract marker: {needle}"
    return value


def pattern_position(source: str, pattern: str) -> int:
    match = re.search(pattern, source, flags=re.MULTILINE | re.DOTALL)
    assert match is not None, f"missing source contract pattern: {pattern}"
    return match.start()


def function_section(source: str, signature: str, next_signature: str | None = None) -> str:
    start = position(source, signature)
    if next_signature is None:
        return source[start:]
    end = source.find(next_signature, start + len(signature))
    assert end >= 0, f"missing next function marker: {next_signature}"
    return source[start:end]


def test_full_recovery_is_not_on_interactive_startup_critical_path():
    assert "object_recovery_run();" not in STARTUP

    services = function_section(
        STARTUP,
        "static void app_startup_initialize_services(",
        "void app_startup_run(void)",
    )
    run = function_section(STARTUP, "void app_startup_run(void)")

    service_patterns = [
        r"ESP_ERROR_CHECK\(\s*object_recovery_run_active\(\)\s*\)",
        r"ESP_ERROR_CHECK\(\s*object_store_init\(\)\s*\)",
        r"ESP_ERROR_CHECK\(\s*ui_init\(\)\s*\)",
        r"keypad_start_event_monitor\(",
        r'app_startup_log_elapsed\(\s*"interactive_ready"',
        r"web_server_start\(\)",
        r"wifi_manager_start\(\)",
        r"seismic_acquisition_start\(\)",
    ]
    service_positions = [pattern_position(services, pattern) for pattern in service_patterns]
    assert service_positions == sorted(service_positions)

    run_patterns = [
        r"web_server_defer_status_cache_refresh\(\)",
        r"app_startup_initialize_services\(\s*startup_started_ms\s*\)",
        r"power_control_arm_shutdown_button\(\)",
        r'app_startup_log_elapsed\(\s*"startup_total"',
        r"object_recovery_start_background_with_callback\(",
    ]
    run_positions = [pattern_position(run, pattern) for pattern in run_patterns]
    assert run_positions == sorted(run_positions)


def test_interactive_boundary_is_local_ui_plus_keypad_not_network():
    services = function_section(
        STARTUP,
        "static void app_startup_initialize_services(",
        "void app_startup_run(void)",
    )

    ui = pattern_position(services, r"ESP_ERROR_CHECK\(\s*ui_init\(\)\s*\)")
    keypad = position(services, "keypad_start_event_monitor(")
    interactive = pattern_position(
        services,
        r'app_startup_log_elapsed\(\s*"interactive_ready"\s*,\s*startup_started_ms',
    )
    web = position(services, "web_server_start()")
    wifi = position(services, "wifi_manager_start()")

    assert ui < keypad < interactive < web < wifi
    assert "uint32_t startup_started_ms" in services


def test_background_recovery_status_is_visible_without_reentering_boot_ui():
    assert "APP_EVENT_STORAGE_RECOVERY_STARTED" in APP_EVENTS
    assert "APP_EVENT_STORAGE_RECOVERY_FINISHED" in APP_EVENTS

    run = function_section(STARTUP, "void app_startup_run(void)")
    startup_total = pattern_position(
        run,
        r'app_startup_log_elapsed\(\s*"startup_total"',
    )
    status_started = position(
        run,
        "APP_EVENT_STORAGE_RECOVERY_STARTED",
    )
    worker_start = position(
        run,
        "object_recovery_start_background_with_callback(",
    )
    assert startup_total < status_started < worker_start

    callback = function_section(
        STARTUP,
        "static void app_startup_background_recovery_complete(",
        "static void app_startup_initialize_core(void)",
    )
    status_finished = position(
        callback,
        "APP_EVENT_STORAGE_RECOVERY_FINISHED",
    )
    cache_enable = position(
        callback,
        "web_server_enable_status_cache_refresh()",
    )
    assert status_finished < cache_enable

    assert "case APP_EVENT_STORAGE_RECOVERY_STARTED:" in UI_NAVIGATION
    assert "context->storage_recovery_active = true;" in UI_NAVIGATION
    assert "context->storage_recovery_failed = false;" in UI_NAVIGATION
    assert "case APP_EVENT_STORAGE_RECOVERY_FINISHED:" in UI_NAVIGATION
    assert "context->storage_recovery_active = false;" in UI_NAVIGATION
    assert "context->storage_recovery_failed = event_value != ESP_OK;" in UI_NAVIGATION

    assert '"CHECKING STORAGE..."' in UI_HOME
    assert '"STORAGE CHECK INCOMPLETE"' in UI_HOME
    assert '"UP/DOWN NAV   OK/RIGHT OPEN"' in UI_HOME


def test_web_archive_cache_waits_for_background_recovery_completion():
    callback = function_section(
        STARTUP,
        "static void app_startup_background_recovery_complete(",
        "static void app_startup_initialize_core(void)",
    )
    run = function_section(STARTUP, "void app_startup_run(void)")

    assert "web_server_enable_status_cache_refresh()" in callback
    assert "app_startup_background_recovery_complete" in run
    assert "object_recovery_start_background_with_callback(" in run

    assert "web_status_cache_arbitration_set_refresh_enabled(false)" in WEB_PREPARE
    assert "web_status_cache_arbitration_set_refresh_enabled(true)" in WEB_PREPARE
    assert "web_status_json_objects_request_refresh()" in WEB_PREPARE

    reserve = function_section(
        WEB_ARBITRATION,
        "bool web_status_cache_arbitration_try_reserve_refresh(",
        "void web_status_cache_arbitration_finish_refresh(",
    )
    gate = position(
        reserve,
        "if (!web_status_cache_arbitration_refresh_enabled())",
    )
    lock = position(
        reserve,
        "if (!web_status_cache_arbitration_lock(arbitration))",
    )
    assert gate < lock


def test_archive_recovery_releases_storage_between_objects():
    guarded_start = position(RECOVERY, "static esp_err_t object_recovery_check_guarded")
    guarded_end = position(RECOVERY, "esp_err_t object_recovery_run_cooperative")
    guarded = RECOVERY[guarded_start:guarded_end]

    begin = position(guarded, "storage_manager_operation_begin()")
    check = position(guarded, "object_recovery_check_one(")
    end = position(guarded, "storage_manager_operation_end()")
    assert begin < check < end

    cooperative = RECOVERY[guarded_end:]
    assert "object_recovery_collect_ids(" in cooperative
    assert "object_recovery_check_guarded(" in cooperative
    assert "OBJECT_RECOVERY_PROGRESS" in cooperative
    assert "vTaskDelay(pdMS_TO_TICKS(1))" in cooperative


def test_directory_census_does_not_hold_guard_during_metadata_loop():
    collect_start = position(RECOVERY, "static esp_err_t object_recovery_collect_ids")
    guarded_start = position(RECOVERY, "static bool object_recovery_should_stop")
    collect = RECOVERY[collect_start:guarded_start]

    assert collect.count("storage_manager_operation_begin()") == 1
    assert collect.count("storage_manager_operation_end()") >= 1
    assert "object_recovery_check_one(" not in collect


def test_background_worker_is_low_priority_psram_shutdown_aware_and_callback_safe():
    assert "APP_TASK_PRIORITY_OBJECT_RECOVERY 1" in TASK_CONFIG
    assert "APP_TASK_STACK_OBJECT_RECOVERY" in TASK_CONFIG
    assert "xTaskCreatePinnedToCoreWithCaps(" in WORKER
    assert "APP_TASK_CORE_SERVICES" in WORKER
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in WORKER
    assert "OBJECT_RECOVERY_BACKGROUND_START_DELAY_MS" in WORKER
    assert "s_stop_requested" in WORKER
    assert "vTaskDeleteWithCaps(NULL)" in WORKER

    task = function_section(
        WORKER,
        "static void object_recovery_background_task(void *argument)",
        "esp_err_t object_recovery_start_background_with_callback(",
    )
    callback = position(task, "complete_fn(err, complete_context)")
    task_clear = position(task, "s_task = NULL;")
    assert callback < task_clear
    assert "if (!stopped && complete_fn != NULL)" in task


def test_shutdown_stops_recovery_before_lvgl_and_storage_teardown():
    wait_and_run = function_section(SHUTDOWN, "void app_shutdown_wait_and_run(void)")
    stop = position(wait_and_run, "object_recovery_stop_background()")
    ui = position(wait_and_run, "ui_shutdown()")
    storage_phase = position(wait_and_run, "app_shutdown_stop_storage()")
    assert stop < ui < storage_phase

    storage_helper = function_section(
        SHUTDOWN,
        "static void app_shutdown_stop_storage(void)",
        "static void app_shutdown_stop_display(void)",
    )
    storage = position(storage_helper, "storage_manager_shutdown()")
    unmount = position(storage_helper, "sd_card_unmount()")
    assert storage < unmount
