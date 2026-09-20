"""Narrow source contracts for the Phase B application event migration."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_app_events_owns_a_dedicated_idf_loop_with_legacy_budget():
    source = _read("components/app_events/app_events.c")
    assert "esp_event_loop_create(" in source
    assert ".queue_size = APP_EVENTS_QUEUE_SIZE" in source
    assert ".task_core_id = APP_EVENTS_TASK_CORE" in source
    assert ".task_priority = APP_EVENTS_TASK_PRIORITY" in source
    assert ".task_stack_size = APP_EVENTS_TASK_STACK_SIZE" in source
    assert "#define APP_EVENTS_QUEUE_SIZE       24" in source
    assert "APP_TASK_CORE_SERVICES" in source
    assert "APP_TASK_PRIORITY_APP_EVENT_LOOP" in source
    assert "APP_TASK_STACK_APP_EVENT_LOOP" in source
    assert "esp_event_loop_create_default" not in source
    assert "xQueue" not in source


def test_app_events_posts_to_the_user_loop_without_waiting():
    source = _read("components/app_events/app_events.c")
    post = source[source.index("esp_event_post_to("):]
    assert "APP_EVENT" in post
    assert "sizeof(payload)" in post
    assert "            0\n" in post or "            0\r\n" in post


def test_event_ids_and_payload_contract_are_preserved():
    header = _read("components/app_events/include/app_events.h")
    expected = [
        "APP_EVENT_KEYPAD = 0",
        "APP_EVENT_WIFI_SETUP_CLIENT_READY",
        "APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED",
        "APP_EVENT_WIFI_SETUP_PAGE_OPENED",
        "APP_EVENT_WIFI_CREDENTIALS_SAVED",
        "APP_EVENT_WIFI_STA_CONNECTING",
        "APP_EVENT_WIFI_STA_CONNECTED",
        "APP_EVENT_WIFI_STA_DISCONNECTED",
        "APP_EVENT_TLS_CA_DOWNLOADED",
        "APP_EVENT_TLS_CA_INSTALL_CONFIRMED",
        "APP_EVENT_TLS_VERIFY_READY",
        "APP_EVENT_TLS_VERIFY_RESULT",
        "APP_EVENT_STATUS_WARNING",
        "APP_EVENT_STATUS_ERROR",
        "APP_EVENT_STATUS_CLEAR_NON_STICKY",
        "APP_EVENT_POWER_SHUTDOWN_REQUESTED",
    ]
    positions = [header.index(item) for item in expected]
    assert positions == sorted(positions)
    assert "esp_err_t app_events_post(" in header
    assert "int32_t value" in header


def test_ui_is_a_subscriber_not_an_event_task():
    source = _read("components/ui/ui_event_dispatcher.c")
    assert "esp_event_handler_register_with(" in source
    assert "esp_event_handler_unregister_with(" in source
    assert "app_events_get_loop()" in source
    assert "xTaskCreate" not in source
    assert "app_event_bus" not in source


def test_startup_defers_web_and_runtime_ui_load_until_wifi_is_running():
    source = _read("main/app_startup.c")
    assert source.index("app_events_init()") < source.index("wifi_manager_init()")
    assert source.index("wifi_manager_start()") < source.index("ui_init()")
    assert source.index("wifi_manager_start()") < source.index("keypad_start_event_monitor(")
    assert source.index("wifi_manager_start()") < source.index("web_server_start()")


def test_application_sources_have_no_legacy_bus_dependency():
    roots = (REPO_ROOT / "components", REPO_ROOT / "main", REPO_ROOT / "tests" / "host")
    matches = []
    for root in roots:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in {".c", ".h", ".json", ".txt", ".md"}:
                if "app_event_bus" in path.read_text(encoding="utf-8", errors="replace"):
                    matches.append(path.relative_to(REPO_ROOT).as_posix())
    assert matches == []
    legacy_component = REPO_ROOT / "components/app_event_bus"
    assert not any(path.is_file() for path in legacy_component.rglob("*")) if legacy_component.exists() else True
