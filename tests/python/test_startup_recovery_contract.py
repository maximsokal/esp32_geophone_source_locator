"""Architecture contracts for fast interactive startup and cooperative recovery.

These are source/ordering contracts, not physical startup-time proof.  The
<=5 s acceptance target still requires a live board log with the real SD card.
"""

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


def position(source: str, needle: str) -> int:
    value = source.find(needle)
    assert value >= 0, f"missing source contract marker: {needle}"
    return value


def test_full_recovery_is_not_on_interactive_startup_critical_path():
    assert "object_recovery_run();" not in STARTUP

    ordered = [
        "object_recovery_run_active()",
        "object_store_init()",
        "ui_init()",
        "keypad_start_event_monitor(",
        "seismic_acquisition_start()",
        "power_control_arm_shutdown_button()",
        '"startup_total"',
        "object_recovery_start_background()",
    ]
    positions = [position(STARTUP, marker) for marker in ordered]
    assert positions == sorted(positions)


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


def test_background_worker_is_low_priority_psram_and_shutdown_aware():
    assert "APP_TASK_PRIORITY_OBJECT_RECOVERY 1" in TASK_CONFIG
    assert "APP_TASK_STACK_OBJECT_RECOVERY" in TASK_CONFIG
    assert "xTaskCreatePinnedToCoreWithCaps(" in WORKER
    assert "APP_TASK_CORE_SERVICES" in WORKER
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in WORKER
    assert "OBJECT_RECOVERY_BACKGROUND_START_DELAY_MS" in WORKER
    assert "s_stop_requested" in WORKER
    assert "vTaskDeleteWithCaps(NULL)" in WORKER


def test_shutdown_stops_recovery_before_lvgl_and_storage_teardown():
    stop = position(SHUTDOWN, "object_recovery_stop_background()")
    ui = position(SHUTDOWN, "ui_shutdown()")
    storage = position(SHUTDOWN, "storage_manager_shutdown()")
    unmount = position(SHUTDOWN, "sd_card_unmount()")
    assert stop < ui < storage < unmount
