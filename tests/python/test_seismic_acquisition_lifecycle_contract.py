from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ACQUISITION = REPO_ROOT / "components" / "seismic_acquisition" / "seismic_acquisition.c"
ACQUISITION_HEADER = REPO_ROOT / "components" / "seismic_acquisition" / "include" / "seismic_acquisition.h"
ACQUISITION_CMAKE = REPO_ROOT / "components" / "seismic_acquisition" / "CMakeLists.txt"
STARTUP = REPO_ROOT / "main" / "app_startup.c"
WEB_CONTROL = REPO_ROOT / "components" / "web_server" / "web_api_control.c"
UI_SCREEN_REGISTRY = REPO_ROOT / "components" / "ui" / "ui_screen_registry.c"


def _between(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    finish = text.index(end, begin)
    return text[begin:finish]


def test_acquisition_lifecycle_uses_task_mutex_and_atomic_read_state():
    source = ACQUISITION.read_text(encoding="utf-8")
    header = ACQUISITION_HEADER.read_text(encoding="utf-8")
    cmake = ACQUISITION_CMAKE.read_text(encoding="utf-8")

    assert "esp_err_t seismic_acquisition_init(void);" in header
    assert "xSemaphoreCreateMutex()" in source
    assert "xSemaphoreTake(" in source
    assert "portMAX_DELAY" in source
    assert "atomic_bool s_running" in source
    assert "atomic_int s_last_error" in source
    assert "atomic_load_explicit" in source
    assert "portENTER_CRITICAL" not in source
    assert "freertos" in cmake


def test_start_and_stop_are_linearized_around_backend_transition():
    source = ACQUISITION.read_text(encoding="utf-8")
    facade_start = _between(source, "esp_err_t seismic_acquisition_start(void)", "static esp_err_t seismic_acquisition_probe_locked")
    assert facade_start.index("seismic_acquisition_lifecycle_lock") < facade_start.index("seismic_acquisition_start_locked()") < facade_start.index("seismic_acquisition_lifecycle_unlock")
    start = _between(source, "static esp_err_t seismic_acquisition_start_locked", "esp_err_t seismic_acquisition_start(void)")
    publish = start.index("seismic_acquisition_publish_status(true)")
    assert start.index("driver->start()") < publish
    running_arg = start.index("&s_running", publish)
    assert "true" in start[running_arg:start.index(");", running_arg)]
    stop = _between(source, "static void seismic_acquisition_stop_locked", "void seismic_acquisition_stop(void)")
    assert stop.index("atomic_exchange_explicit") < stop.index("driver->stop()")
    assert "if (!preserve_buffer)" in stop
    shutdown = _between(source, "void seismic_acquisition_prepare_shutdown", "bool seismic_acquisition_is_running")
    assert shutdown.index("s_shutdown = true") < shutdown.index("seismic_acquisition_stop_locked(true)")


def test_boot_initializes_lifecycle_but_does_not_auto_start_acquisition():
    """Software init precedes services; bounded power-gated preflight is separate."""
    startup = STARTUP.read_text(encoding="utf-8")
    core = _between(startup, "static void app_startup_initialize_core", "static void app_startup_initialize_services")
    services = _between(startup, "static void app_startup_initialize_services", "void app_startup_run")

    assert core.index("seismic_buffer_init()") < core.index("seismic_acquisition_init()") < core.index("adc_service_init()")

    assert "seismic_acquisition_start()" not in services
    assert services.index("ina3221_monitor_start()") < services.index("seismic_acquisition_probe()")

    web_start = services.index("web_server_start()")
    wifi_start = services.index("wifi_manager_start()")
    interactive = services.index('"interactive_ready"')
    assert wifi_start < web_start < interactive


def test_workflow_probes_and_only_live_measurements_owns_preview():
    registry = UI_SCREEN_REGISTRY.read_text(encoding="utf-8")
    route = _between(registry, "void ui_screen_open_locked(", "void ui_screen_render_locked(")
    assert "seismic_acquisition_start()" not in route
    assert "seismic_acquisition_probe()" in route
    assert "seismic_acquisition_set_preview(SEISMIC_PREVIEW_TFT, true)" in route
    assert "seismic_acquisition_set_preview(SEISMIC_PREVIEW_TFT, false)" in route


def test_web_measurement_start_checks_acquisition_before_queuing():
    """Architecture guard only; runtime failure behavior is covered by the
    web_api_control_http host test against the real handler."""
    source = WEB_CONTROL.read_text(encoding="utf-8")
    helper = _between(
        source,
        "static esp_err_t web_control_measurement_start",
        "static esp_err_t web_control_acquisition_stop",
    )
    acquisition_start = helper.index("seismic_acquisition_start()")
    acquisition_failure = helper.index("if (acquisition_err != ESP_OK)")
    failure_return = helper.index("return acquisition_err;", acquisition_failure)
    async_start = helper.index("measurement_service_start_object_async(object_id)")
    assert acquisition_start < acquisition_failure < failure_return < async_start


def test_web_stop_keeps_measurement_guard_before_serialized_acquisition_stop():
    source = WEB_CONTROL.read_text(encoding="utf-8")
    helper = _between(
        source,
        "static esp_err_t web_control_acquisition_stop",
        "static esp_err_t web_control_process",
    )

    assert helper.index("measurement_service_snapshot_blocks_acquisition_stop") < helper.index(
        "seismic_acquisition_set_preview(SEISMIC_PREVIEW_WEB, false)"
    )
