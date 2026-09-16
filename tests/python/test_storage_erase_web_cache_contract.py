from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _function(source: str, signature: str, next_signature: str | None = None) -> str:
    start = source.index(signature)
    if next_signature is None:
        return source[start:]
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def test_storage_service_has_independent_content_observer() -> None:
    public = _source("components/storage_service/include/storage_service.h")
    internal = _source("components/storage_service/storage_service_internal.h")
    observer = _source("components/storage_service/storage_service_content_observer.c")

    assert "storage_service_event_callback_t" in public
    assert "storage_service_content_changed_callback_t" in public
    assert "storage_service_set_content_changed_observer" in public
    assert "storage_service_content_observer_slot_t" in internal
    assert "in_flight" in observer
    assert "storage_service_wait_for_content_observer" in observer
    assert "s_content_observer.callback = NULL" in observer
    assert "storage_service_wait_for_content_observer();" in observer


def test_successful_erase_publishes_independently_of_ui_notify() -> None:
    service = _source("components/storage_service/storage_service.c")
    task = _function(
        service,
        "static void storage_service_task(",
        "static esp_err_t storage_service_submit(",
    )

    content_call = task.index("storage_service_internal_notify_content_changed();")
    ui_branch = task.index("if (notify)")
    assert content_call < ui_branch
    assert "command == STORAGE_COMMAND_ERASE_DATA" in task[:content_call]
    assert "s_event.type == STORAGE_SERVICE_EVENT_ERASE_DONE" in task[:content_call]


def test_storage_shutdown_drains_content_observer() -> None:
    service = _source("components/storage_service/storage_service.c")
    shutdown = _function(
        service,
        "void storage_service_shutdown(void)",
        "esp_err_t storage_service_set_event_callback(",
    )
    assert "storage_service_internal_clear_content_observer();" in shutdown
    assert shutdown.index("storage_service_internal_clear_content_observer();") < shutdown.index("if (!s_initialized)")


def test_web_cache_registers_and_unregisters_observer_before_free() -> None:
    web = _source("components/web_server/web_status_json_objects.c")
    init = _function(
        web,
        "esp_err_t web_status_json_objects_init(void)",
        "void web_status_json_objects_shutdown(void)",
    )
    shutdown = _function(
        web,
        "void web_status_json_objects_shutdown(void)",
        "esp_err_t web_status_json_objects_request_refresh(void)",
    )

    assert "storage_service_set_content_changed_observer(" in init
    assert "web_status_json_objects_storage_changed" in init
    unregister = shutdown.index("storage_service_set_content_changed_observer(NULL, NULL)")
    free_payload = shutdown.index("heap_caps_free(s_payload_a)")
    assert unregister < free_payload


def test_component_and_host_wiring() -> None:
    storage_cmake = _source("components/storage_service/CMakeLists.txt")
    web_manifest = _source("components/web_server/idf_component.yml")
    host_manifest = _source("tests/host/manifest.d/coverage-services.json")

    assert '"storage_service_content_observer.c"' in storage_cmake
    assert "storage_service:" in web_manifest
    assert "path: ../storage_service" in web_manifest
    assert "require: private" in web_manifest
    assert "storage_service_content_observer.c" in host_manifest
    assert "storage_service_lifecycle_test.c" in host_manifest


def test_max59_storage_busy_contract_remains() -> None:
    api = _source("components/web_server/web_api_status.c")
    assert "err == ESP_ERR_INVALID_STATE" in api
    assert '"503 Service Unavailable"' in api
    assert '{\\"storage_busy\\":true}' in api
