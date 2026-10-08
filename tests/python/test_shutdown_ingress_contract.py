"""Static lifecycle contracts for MAX-55 shutdown ingress/drain ordering."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def _between(source: str, start: str, end: str) -> str:
    start_index = source.index(start)
    end_index = source.index(end, start_index)
    return source[start_index:end_index]


def test_keypad_gate_is_readable_and_closed_before_shutdown_latch():
    header = _read("components/app_events/include/app_events.h")
    events = _read("components/app_events/app_events.c")
    power = _read("components/power_control/power_button_adapter.c")

    assert "bool app_events_keypad_delivery_enabled(void);" in header
    getter = _between(
        events,
        "bool app_events_keypad_delivery_enabled(void)",
        "static esp_err_t app_events_post_internal",
    )
    assert "memory_order_acquire" in getter

    long_press = _between(
        power,
        "event != BUTTON_LONG_PRESS_START",
        "const uint32_t latch_ms",
    )
    assert long_press.index("app_events_set_keypad_delivery_enabled(false)") < long_press.index(
        "POWER_EVENT_SHUTDOWN_REQUESTED"
    )


def test_ui_drops_pending_keypad_after_gate_close_but_keeps_other_events():
    source = _read("components/ui/ui_event_dispatcher.c")
    keypad = _between(
        source,
        "if (event.event_id == APP_EVENT_KEYPAD)",
        "else {\n            ui_dispatch_app_event_locked",
    )
    assert "!app_events_keypad_delivery_enabled()" in keypad
    assert keypad.index("!app_events_keypad_delivery_enabled()") < keypad.index(
        "ui_dispatch_key_locked"
    )
    assert "ui_dispatch_app_event_locked(" in source


def test_web_component_depends_on_power_control_and_policy_fails_closed():
    cmake = _read("components/web_server/CMakeLists.txt")
    header = _read("components/web_server/web_server_lifecycle_policy.h")
    internal = _read("components/web_server/web_server_internal.h")

    requires = _between(cmake, "    REQUIRES\n", "    EMBED_FILES")
    assert "        power_control\n" in requires
    assert "return !shutdown_requested;" in header
    assert "web_lifecycle_small_body_timeout_retry_allowed" in header
    assert "return false;" in header
    assert "power_control_is_shutdown_requested" in internal
    assert "web_server_shutdown_requested" in internal


def test_control_admission_precedes_scan_and_body_is_bounded_and_shutdown_aware():
    source = _read("components/web_server/web_api_control.c")
    handler = source[source.index("static esp_err_t web_api_control_handler"):]
    assert handler.index("web_server_shutdown_requested()") < handler.index(
        "web_control_scan_try_handle"
    )

    reader = _between(
        source,
        "static esp_err_t web_control_read_body",
        "static esp_err_t web_control_send_result",
    )
    assert reader.count("web_server_shutdown_requested()") >= 2
    timeout = reader[reader.index("received == HTTPD_SOCK_ERR_TIMEOUT"):]
    assert "return ESP_ERR_TIMEOUT;" in timeout

    action_copy = handler.index("memcpy(action_copy")
    final_gate = handler.index("web_server_shutdown_requested()", action_copy)
    process = handler.index("web_control_process(", final_gate)
    assert final_gate < process


def test_scan_upload_keeps_bulk_timeout_policy_and_finishes_fully_accepted_work():
    source = _read("components/web_server/web_scan_store.c")
    assert "#define WEB_SCAN_RECV_TIMEOUTS    5U" in source

    save = _between(
        source,
        "static esp_err_t web_scan_save",
        "static esp_err_t web_scan_open_for_stream",
    )
    recv_index = save.index("httpd_req_recv(")
    assert save.rfind("web_server_shutdown_requested()", 0, recv_index) >= 0
    post_recv_gate = save.index("web_server_shutdown_requested()", recv_index)
    write_index = save.index("web_scan_write_chunk", recv_index)
    assert post_recv_gate < write_index

    close_index = save.index("web_scan_close_writer")
    commit_index = save.index("web_scan_commit_scan_temp", close_index)
    assert "web_server_shutdown_requested()" not in save[close_index:commit_index]
    assert "shutdown admission boundary" in save[close_index:commit_index]
    assert "web_scan_remove_temp_best_effort(temp_path)" in save


def test_wifi_setup_rechecks_latch_before_credentials_and_radio_transition():
    source = _read("components/web_server/web_wifi_setup.c")
    handler = _between(
        source,
        "static esp_err_t web_wifi_setup_post_handler",
        "esp_err_t web_wifi_setup_register",
    )
    assert handler.index("web_server_shutdown_requested()") < handler.index(
        "web_wifi_setup_request_read_body"
    )

    parse_index = handler.index("web_wifi_setup_form_parse")
    save_index = handler.index("wifi_manager_save_sta_credentials")
    assert handler.index("web_server_shutdown_requested()", parse_index) < save_index

    delay_index = handler.index("vTaskDelay(")
    connect_index = handler.index("wifi_manager_connect_saved")
    assert handler.rfind("web_server_shutdown_requested()", 0, delay_index) >= 0
    assert handler.index("web_server_shutdown_requested()", delay_index) < connect_index
    assert '.uri = "/api/wifi/setup"' in source


def test_other_small_body_readers_remain_fail_on_timeout_or_error():
    wifi_reader = _read("components/web_server/web_wifi_setup_request.c")
    client_log = _read("components/web_server/web_client_log.c")
    onboarding = _read("components/web_server/web_https_onboarding.c")

    assert "if (result <= 0)" in wifi_reader
    assert "continue;" not in wifi_reader
    assert "received == HTTPD_SOCK_ERR_TIMEOUT" in client_log
    assert "? ESP_ERR_TIMEOUT" in client_log

    report_start = onboarding.index("static esp_err_t web_onboarding_read_report")
    report_tail = onboarding[report_start:]
    assert "if (result <= 0)" in report_tail
    assert "continue;" not in _between(
        report_tail,
        "static esp_err_t web_onboarding_read_report",
        "static bool web_onboarding_json_true",
    )


def test_shutdown_drains_recovery_and_network_before_dependent_services():
    source = _read("main/app_shutdown.c")
    body = source[source.index("void app_shutdown_wait_and_run(void)"):]

    order = [
        "keypad_stop_event_monitor();",
        "object_recovery_stop_background();",
        "app_shutdown_stop_network();",
        "nrf_topology_service_stop();",
        "sensor_bus_deinit();",
        "measurement_service_shutdown();",
        "seismic_recorder_stop();",
        "seismic_recorder_wait_for_stop();",
        "ui_shutdown();",
        "object_service_shutdown();",
        "seismic_acquisition_stop();",
        "app_shutdown_stop_storage();",
    ]
    positions = [body.index(token) for token in order]
    assert positions == sorted(positions)
    assert body.count("app_shutdown_stop_network();") == 1


def test_insights_shutdown_precedes_web_and_wifi_teardown():
    source = _read("main/app_shutdown.c")
    helper = _between(
        source,
        "static void app_shutdown_stop_network(void)",
        "static void app_shutdown_stop_storage(void)",
    )
    assert helper.index("insights_bridge_shutdown();") < helper.index("web_server_stop();")
    assert helper.index("web_server_stop();") < helper.index("wifi_manager_stop();")


def test_web_stopped_plan_keeps_https_http_exit_order():
    source = _read("components/web_server/web_server_lifecycle_policy.c")
    stopped = _between(
        source,
        "static const web_lifecycle_action_t stopped[]",
        "const web_lifecycle_action_t *plan",
    )
    assert stopped.index("WEB_LIFECYCLE_ACTION_HTTPS_STOP") < stopped.index(
        "WEB_LIFECYCLE_ACTION_HTTP_STOP"
    ) < stopped.index("WEB_LIFECYCLE_ACTION_EXIT")
