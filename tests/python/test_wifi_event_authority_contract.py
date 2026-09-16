from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_sta_event_state_is_synchronized_and_live_validated() -> None:
    source = read("components/wifi_manager/wifi_manager_sta_events.c")
    assert "portMUX_TYPE s_state_lock" in source
    assert "wifi_manager_sta_event_state_t s_state" in source
    assert "esp_wifi_sta_get_ap_info" in source
    assert "esp_netif_get_ip_info" in source
    assert "wifi_manager_platform_sta_netif" in source
    assert "wifi_manager_sta_ip_observation_accept" in source
    assert "wifi_manager_sta_disconnect_observation_accept" in source
    assert "(void)event_data;" in source
    assert "event->ip_info.ip" not in source


def test_start_paths_bracket_event_admission_and_retire_both_lifetimes() -> None:
    sta = read("components/wifi_manager/wifi_manager_sta.c")
    setup = read("components/wifi_manager/wifi_manager_setup_ap.c")

    assert sta.index("esp_wifi_set_mode") < sta.index(
        "wifi_manager_sta_events_set_connect_enabled"
    ) < sta.index("esp_wifi_start")
    assert "if (err != ESP_OK) {\n        wifi_manager_sta_events_reset();" in sta
    assert "wifi_manager_setup_events_reset();" in sta

    assert "wifi_manager_sta_events_set_connect_enabled(\n        false" in setup
    assert setup.index("wifi_manager_setup_events_set_enabled") < setup.index(
        "esp_wifi_start"
    )
    assert "if (err != ESP_OK) {\n        wifi_manager_setup_events_reset();" in setup


def test_setup_events_validate_current_softap_membership() -> None:
    source = read("components/wifi_manager/wifi_manager_setup_events.c")
    assert "portMUX_TYPE s_state_lock" in source
    assert "esp_wifi_ap_get_sta_list" in source
    assert "wifi_manager_setup_client_observation_accept" in source
    assert "wifi_manager_setup_disconnect_observation_accept" in source
    assert "event MAC is not currently associated" in source


def test_final_network_authority_is_separate_from_transaction_lock() -> None:
    manager = read("components/wifi_manager/wifi_manager.c")
    mode = read("components/wifi_manager/wifi_manager_mode.c")

    assert "s_state.network_events_suppressed = true;" in manager
    assert "void wifi_manager_internal_commit_final_state" in manager
    assert "s_state.network_events_suppressed = false;" in manager

    # Every completion notification in mode transitions follows a final-state
    # publication in the same path. Intermediate stopped state remains hidden.
    assert mode.count("wifi_manager_internal_commit_final_state(") >= 5
    assert "wifi_manager_internal_commit_state(\n        false,\n        false,\n        false\n    );" in mode

    abort = mode.index("static esp_err_t wifi_manager_abort_mode_transition")
    recover = mode.index("static esp_err_t wifi_manager_recover_uncertain_radio_state")
    abort_body = mode[abort:recover]
    assert abort_body.index("wifi_manager_internal_commit_final_state") < abort_body.index(
        "wifi_manager_notify_mode_transition_complete"
    )

    setup_start = mode.index("esp_err_t wifi_manager_enter_setup_mode")
    connect_start = mode.index("esp_err_t wifi_manager_connect_saved")
    stop_start = mode.index("esp_err_t wifi_manager_stop(void)")
    setup_body = mode[setup_start:connect_start]
    connect_body = mode[connect_start:stop_start]
    stop_body = mode[stop_start:]
    for body in (setup_body, connect_body, stop_body):
        assert body.index("wifi_manager_internal_commit_final_state") < body.index(
            "wifi_manager_notify_mode_transition_complete"
        )


def test_web_raw_events_resolve_from_current_wifi_authority() -> None:
    policy = read("components/web_server/web_server_lifecycle_policy.c")
    server = read("components/web_server/web_server.c")

    for name in (
        "wifi_manager_is_running",
        "wifi_manager_is_setup_mode",
        "wifi_manager_is_radio_state_uncertain",
        "wifi_manager_network_events_suppressed",
        "wifi_manager_sta_has_ip",
    ):
        assert name in policy
    assert "wifi_manager_is_transition_active" not in policy
    assert "WEB_LIFECYCLE_STATE_QUIESCED" in policy
    assert "web_lifecycle_effective_network_state" in server


def test_tft_rechecks_queued_wifi_events_against_current_authority() -> None:
    navigation = read("components/ui/ui_navigation.c")
    policy = read("components/ui/ui_event_policy.h")

    assert "ui_event_setup_client_notice_allowed" in policy
    assert "ui_event_sta_notice_allowed" in policy
    assert "ui_setup_client_event_is_authoritative" in navigation
    assert "ui_sta_event_is_authoritative" in navigation
    assert "wifi_manager_network_events_suppressed" in navigation
    assert "wifi_manager_sta_has_ip" in navigation
    assert navigation.count("ui_setup_client_event_is_authoritative()") >= 2
    assert navigation.count("ui_sta_event_is_authoritative()") >= 3

    # Connecting/disconnected notices are rejected if current runtime already
    # owns an IP; connected is rejected if no current IP exists.
    connecting = navigation.index("case APP_EVENT_WIFI_STA_CONNECTING:")
    connected = navigation.index("case APP_EVENT_WIFI_STA_CONNECTED:")
    disconnected = navigation.index("case APP_EVENT_WIFI_STA_DISCONNECTED:")
    tls = navigation.index("case APP_EVENT_TLS_CA_DOWNLOADED:")
    assert "wifi_manager_sta_has_ip()" in navigation[connecting:connected]
    assert "!wifi_manager_sta_has_ip()" in navigation[connected:disconnected]
    assert "wifi_manager_sta_has_ip()" in navigation[disconnected:tls]
