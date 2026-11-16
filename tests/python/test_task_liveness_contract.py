"""Static contracts for post-operation FreeRTOS task liveness beats."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


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


def _positions(source: str, needle: str) -> list[int]:
    positions: list[int] = []
    offset = 0
    while True:
        position = source.find(needle, offset)
        if position < 0:
            return positions
        positions.append(position)
        offset = position + len(needle)


def test_ui_event_handler_is_nonblocking_and_lvgl_drains_events():
    source = (REPO_ROOT / "components/ui/ui_event_dispatcher.c").read_text(encoding="utf-8")
    handler = _function_body(source, "static void ui_event_handler")
    ingress = _function_body(source, "static ui_event_ingress_result_t ui_event_ingress_begin")

    assert "runtime_observability_beat(" not in handler
    assert "ui_event_ingress_begin(" in handler
    assert "lvgl_port_task_wake(" in handler
    assert "lvgl_port_lock(" not in handler
    assert "vTaskDelay(" not in handler
    assert "portENTER_CRITICAL(&s_pending_lock)" in ingress
    assert "s_pending_events[s_pending_tail]" in ingress
    assert "s_pending_count++" in ingress
    assert "vTaskDelay(" not in ingress

    dispatcher = _function_body(
        source,
        "bool ui_event_dispatcher_dispatch_pending_locked"
    )
    dispatch = dispatcher.index("ui_dispatch_")
    observe = dispatcher.index("ui_event_observe();")
    assert observe > dispatch


def test_button_migration_has_no_input_tasks_or_polling_workers():
    keypad = (REPO_ROOT / "components/keypad/keypad_button_adapter.c").read_text(encoding="utf-8")
    power = (REPO_ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")
    arm = _function_body(power, "esp_err_t power_button_adapter_arm_after_startup")

    assert "iot_button_create" in keypad
    assert "button_driver_t" in keypad
    assert "adc_service_read_average_raw" in keypad
    assert "adc_service_try_read_average_raw" in keypad
    assert "button_adc.h" not in keypad
    assert "adc_oneshot_read" not in keypad
    assert "BUTTON_PRESS_DOWN" in keypad
    assert "xTaskCreate" not in keypad
    assert "vTaskDelay" not in keypad
    assert "iot_button_new_gpio_device" in power
    assert "BUTTON_LONG_PRESS_START" in power
    assert "xTaskCreate" not in power

    # A single bounded startup release-confirm delay is allowed. It is not a
    # polling worker and no application-owned input task is created.
    assert power.count("vTaskDelay(") == 1
    assert "vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_RELEASE_CONFIRM_MS));" in arm
    assert "one-time startup delay" in power
    assert "while (" not in arm
    assert "for (" not in arm


def test_button_contracts_preserve_adc_and_power_safety_boundaries():
    keypad = (REPO_ROOT / "components/keypad/keypad_button_adapter.c").read_text(encoding="utf-8")
    power = (REPO_ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")
    keypad_level = _function_body(keypad, "static uint8_t keypad_button_get_key_level")

    assert keypad.count("err = iot_button_create(") == 1
    assert keypad.count(".get_key_level = keypad_button_get_key_level") == 1
    assert "adc_service_try_read_average_raw" in keypad_level
    assert "adc_service_read_average_raw" not in keypad_level
    assert "portMAX_DELAY" not in keypad_level
    assert "s_adc_cache_time_us = esp_timer_get_time()" in keypad_level
    assert "adc_service_handle" not in keypad
    assert "ADC_ATTEN_DB_12" in keypad
    decoder = (REPO_ROOT / "components/keypad/keypad_decode.c").read_text(encoding="utf-8")
    assert '#include "keypad_decode.h"' in keypad
    assert "KEYPAD_SW1_MAX_RAW" in decoder
    assert "KEYPAD_SW5_MAX_RAW" in decoder
    assert "KEYPAD_DECODE_DEADBAND_RAW" in decoder

    latch = power.index("xEventGroupSetBits(state->events, POWER_EVENT_SHUTDOWN_REQUESTED)")
    hook = power.index("state->notification_hook(state->notification_context)")
    notification = power.index("app_events_post_best_effort(")
    assert latch < hook
    assert latch < notification
    assert ".active_level = 1U" in power
    assert ".disable_pull = true" in power
    assert "POWER_BUTTON_LONG_PRESS_MS 150U" in power


def test_acquisition_heartbeat_follows_read_and_buffer_publication():
    source = (REPO_ROOT / "components/ad7606_acquisition/ad7606_acquisition.c").read_text(encoding="utf-8")
    task = _function_body(source, "static void ad7606_worker(")
    publish_helper = _function_body(source, "static esp_err_t ad7606_publish_sample(")

    beats = _positions(task, "runtime_observability_beat(")
    read = task.index("ad7606_driver_read_result(")
    publish = task.index("ad7606_publish_sample(")
    assert read < publish
    assert any(beat > publish for beat in beats)
    push = publish_helper.index("seismic_buffer_push_indexed(")
    # The heartbeat is taken once per wake-up by the worker loop, after the sample was published; a second
    # beat inside the helper doubled the esp_timer reads and spinlock entries on every sample
    # (docs/acquisition-ui-load-plan-2026-11-01.md, WP4).
    assert "runtime_observability_beat(" not in publish_helper
    assert push < len(publish_helper)
    assert max(beats) > publish
