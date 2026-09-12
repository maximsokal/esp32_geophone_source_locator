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

    assert "runtime_observability_beat(" not in handler
    assert "ui_event_pending_push(" in handler
    assert "lvgl_port_task_wake(" in handler
    assert "lvgl_port_lock(" not in handler
    assert "vTaskDelay(" not in handler

    dispatcher = _function_body(
        source,
        "void ui_event_dispatcher_dispatch_pending_locked"
    )
    dispatch = dispatcher.index("ui_dispatch_")
    observe = dispatcher.index("ui_event_observe();")
    assert observe > dispatch


def test_button_migration_has_no_input_tasks_or_polling_workers():
    keypad = (REPO_ROOT / "components/keypad/keypad_button_adapter.c").read_text(encoding="utf-8")
    power = (REPO_ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")

    assert "iot_button_new_adc_device" in keypad
    assert "BUTTON_PRESS_DOWN" in keypad
    assert "xTaskCreate" not in keypad
    assert "vTaskDelay" not in keypad
    assert "iot_button_new_gpio_device" in power
    assert "BUTTON_LONG_PRESS_START" in power
    assert "xTaskCreate" not in power
    assert "vTaskDelay" not in power


def test_button_contracts_preserve_adc_and_power_safety_boundaries():
    keypad = (REPO_ROOT / "components/keypad/keypad_button_adapter.c").read_text(encoding="utf-8")
    power = (REPO_ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")

    assert keypad.count("iot_button_new_adc_device") == 1
    assert keypad.count("button_index = (uint8_t)i") == 1
    assert ".adc_handle = adc_service_handle()" in keypad
    assert "ADC_ATTEN_DB_12" in keypad
    assert all(token in keypad for token in (
        "KEYPAD_LEFT_MAX_MV",
        "KEYPAD_UP_MAX_MV",
        "KEYPAD_DOWN_MAX_MV",
        "KEYPAD_RIGHT_MAX_MV",
        "KEYPAD_OK_MAX_MV",
    ))

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

    beats = _positions(task, "runtime_observability_beat(")
    assert len(beats) == 2
    read = task.index("ad7606_driver_read(")
    publish = task.index("seismic_buffer_push(")
    assert beats[0] > read
    assert beats[1] > publish
