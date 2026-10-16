from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[2]
BOOT = (ROOT / "main/app_storage_boot.c").read_text(encoding="utf-8")
STARTUP = (ROOT / "main/app_startup.c").read_text(encoding="utf-8")
BUTTON = (ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")
KEYPAD = (ROOT / "components/keypad/keypad_button_adapter.c").read_text(encoding="utf-8")
UI = (ROOT / "components/ui/ui.c").read_text(encoding="utf-8")
RECOVERY_UI = (ROOT / "main/app_storage_boot_ui.c").read_text(encoding="utf-8")
I18N_ENTRIES = json.loads(
    (ROOT / "i18n/tft.json").read_text(encoding="utf-8")
)["entries"]


def test_boot_failure_arms_before_wait_and_cleanup():
    failure = BOOT.split("static void app_storage_boot_failure", 1)[1].split(
        "void app_storage_boot_run_diagnostic", 1
    )[0]
    arm = failure.index("power_control_arm_shutdown_button")
    wait = failure.index("power_control_wait_for_shutdown_request")
    unmount = failure.index("sd_card_unmount")
    shutdown = failure.index("power_control_shutdown")
    assert arm < wait < unmount < shutdown
    assert "if (arm_err == ESP_OK)" in failure
    assert "skipping unreachable wait" in failure


def test_boot_failure_does_not_duplicate_button_state_machine():
    assert "gpio_" not in BOOT
    assert "iot_button" not in BOOT
    assert "BUTTON_LONG_PRESS" not in BOOT


def test_normal_startup_arm_boundary_is_not_moved_early():
    run = STARTUP[STARTUP.index("void app_startup_run(void)") :]
    diagnostic = run.index("app_storage_boot_run_diagnostic();")
    services = run.index("app_startup_initialize_services(")
    arm = run.index("power_control_arm_shutdown_button()")
    assert diagnostic < services < arm
    assert run.count("power_control_arm_shutdown_button()") == 1


def test_recovery_ui_registration_precedes_diagnostic_and_init_is_shared():
    run = STARTUP[STARTUP.index("void app_startup_run(void)") :]
    registration = run.index("app_storage_boot_ui_register();")
    diagnostic = run.index("app_storage_boot_run_diagnostic();")
    assert registration < diagnostic

    services = STARTUP.split("static void app_startup_initialize_services", 1)[1].split(
        "void app_startup_run(void)", 1
    )[0]
    assert "app_storage_boot_ui_ensure_initialized()" in services
    services_code = re.sub(r"/\*.*?\*/|//[^\n]*", "", services, flags=re.S)
    assert "display_init()" not in services_code
    assert "ui_boot_init()" not in services_code

    boot_init = UI.split("esp_err_t ui_boot_init(void)", 1)[1].split(
        "void ui_boot_set_stage", 1
    )[0]
    assert "if (s_context.port_ready)" in boot_init
    assert boot_init.index("if (s_context.port_ready)") < boot_init.index("memset(&s_context")


def test_storage_manager_failure_enters_confirmed_recovery_and_retries_once():
    services = STARTUP.split("static void app_startup_initialize_services", 1)[1].split(
        "void app_startup_run(void)", 1
    )[0]
    assert "esp_err_t storage_err = storage_manager_init();" in services
    assert "app_storage_boot_recover_storage_manager_failure(storage_err);" in services
    assert "ESP_ERROR_CHECK(\n        storage_manager_init()" not in services

    recovery = BOOT.split(
        "static app_storage_boot_recovery_result_t app_storage_boot_recover(", 1
    )[1].split(
        "void app_storage_boot_run_diagnostic", 1
    )[0]
    assert "app_storage_boot_policy_classify(&input)" in recovery
    assert "APP_STORAGE_BOOT_ACTION_OFFER_FORMAT" in recovery
    assert "sd_card_format_fat32_task_start(" in recovery
    assert "                true\n" in recovery
    assert "sd_card_format_fat32_task_get_status(&status)" in recovery


def test_recovery_hook_receives_reason_and_never_formats_without_confirmation():
    header = (ROOT / "main/app_storage_boot.h").read_text(encoding="utf-8")
    policy = (ROOT / "main/app_storage_boot_policy.c").read_text(encoding="utf-8")
    assert "show_reason" in header
    assert "wait_decision" in header
    assert "show_progress" in header
    assert "show_result" in header
    assert '"%s err=%s fr=%d"' in policy
    assert 'stage_name = "FS_MOUNT"' in policy
    assert "SD_CARD_FORMAT_STATE_DONE" in BOOT

    layout_recovery = BOOT.split("void app_storage_boot_recover_storage_manager_failure", 1)[1]
    assert layout_recovery.count("storage_manager_init()") == 1


def test_held_button_release_confirmation_contract_remains_in_adapter():
    assert "arm_requested" in BUTTON
    assert "BUTTON_PRESS_UP" in BUTTON
    assert "power_button_adapter_arm_after_startup" in BUTTON
    assert "Shutdown arm deferred: button is held" in BUTTON
    assert "atomic_exchange_explicit" in BUTTON


def test_format_failure_keeps_retry_prompt_active_until_ok_or_poweroff():
    recovery = BOOT.split(
        "static app_storage_boot_recovery_result_t app_storage_boot_recover(", 1
    )[1].split(
        "void app_storage_boot_run_diagnostic", 1
    )[0]
    assert "s_recovery_ui.show_result(s_recovery_ui.context, false, err);" in recovery
    assert (
        "if (success) {\n                hw_inventory_report(HW_MODULE_SD_CARD, HW_STATE_PRESENT, \"ready\");\n"
        "                return APP_STORAGE_BOOT_RECOVERY_SUCCEEDED;\n            }\n            continue;"
    ) in recovery
    assert "continue;" in recovery

    assert "s_format_retry_pending = !success;" in RECOVERY_UI
    assert "UI_I18N_KEY_BOOT_FORMAT_RETRY_HINT" in RECOVERY_UI
    assert "UI_I18N_KEY_BOOT_FORMAT_POWER_HINT" in RECOVERY_UI
    assert "if (s_format_retry_pending && item.key == KEYPAD_KEY_RIGHT)" in RECOVERY_UI


def test_format_retry_hints_have_english_and_uk_translations():
    assert I18N_ENTRIES["boot.format_retry_hint"]["en"] == "OK: RETRY FORMAT"
    assert I18N_ENTRIES["boot.format_retry_hint"]["uk"] == (
        "OK: \u041f\u041e\u0412\u0422\u041e\u0420\u0418\u0422\u0418"
    )
    assert I18N_ENTRIES["boot.format_power_hint"]["en"] == (
        "POWER OFF: INSERT CARD"
    )
    assert I18N_ENTRIES["boot.format_power_hint"]["uk"] == (
        "\u0412\u0418\u041c\u041a\u041d\u0406\u0422\u042c: "
        "\u0417\u041c\u0406\u041d\u0406\u0422\u042c "
        "\u041a\u0410\u0420\u0422\u0423"
    )


def test_boot_key_callback_uses_adapter_release_gate_without_global_keypad_gate():
    callback = KEYPAD.split("static void keypad_button_callback", 1)[1].split(
        "esp_err_t keypad_button_adapter_init", 1
    )[0]
    assert "boot_callback(key, boot_context);" in callback
    assert callback.index("boot_callback(key, boot_context);") < callback.index("app_events_post(")

    key_level = KEYPAD.split("static uint8_t keypad_button_get_key_level", 1)[1].split(
        "static esp_err_t keypad_button_driver_delete", 1
    )[0]
    assert "s_release_armed = false;" in KEYPAD
    assert "raw >= KEYPAD_RELEASE_MIN_RAW" in key_level
    assert "raw <= 4095" in key_level
    assert "KEYPAD_RELEASE_CONFIRM_SAMPLES" in key_level
    assert "!s_release_armed" in key_level
    assert "decoded_key = KEYPAD_KEY_NONE;" in key_level


def test_recovery_prompt_routes_shutdown_hook_to_decision_queue_and_cleans_up():
    assert "power_control_set_shutdown_notification_hook(\n        app_storage_boot_ui_shutdown_callback,\n        s_key_queue\n    )" in RECOVERY_UI
    assert "item.shutdown_notification = true;" in RECOVERY_UI
    assert "app_storage_boot_ui_send_event(queue, &item) != pdTRUE" in RECOVERY_UI
    assert "xQueueSend(queue, item, 0U)" in RECOVERY_UI
    assert "power_control_set_shutdown_notification_hook(NULL, NULL);" in RECOVERY_UI
    assert "keypad_stop_boot_event_monitor();" in RECOVERY_UI
    assert "if (item.shutdown_notification)" in RECOVERY_UI
    assert "return APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;" in RECOVERY_UI
    assert "app_storage_boot_ui_map_key(action, item.key, true, &decision)" in RECOVERY_UI
    assert "Do not interrupt formatting" in RECOVERY_UI
