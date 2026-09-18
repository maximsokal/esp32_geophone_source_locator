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
    start = _between(source, "esp_err_t seismic_acquisition_start", "void seismic_acquisition_stop")
    stop = _between(source, "void seismic_acquisition_stop", "bool seismic_acquisition_is_running")

    assert start.index("seismic_acquisition_lifecycle_lock") < start.index("driver->start()")

    publish = start.index("seismic_acquisition_publish_status(true)")
    running_arg = start.index("&s_running", publish)
    running_store = start.rfind("atomic_store_explicit", publish, running_arg)
    running_store_end = start.index(");", running_arg)
    running_store_text = start[running_store:running_store_end]
    assert "true" in running_store_text
    assert publish < running_store < start.rindex("seismic_acquisition_lifecycle_unlock")

    exchange = stop.index("atomic_exchange_explicit")
    backend_stop = stop.index("driver->stop()")
    unlock = stop.rindex("seismic_acquisition_lifecycle_unlock")
    assert exchange < backend_stop < unlock


def test_boot_initializes_lifecycle_but_does_not_auto_start_acquisition():
    """AD7606 sampling must not run unattended: the driver is brought up at
    boot (seismic_acquisition_init), but active acquisition only starts once
    something actually consumes samples (local Measurements screen entry, or
    an explicit Web request) rather than unconditionally before Wi-Fi."""
    startup = STARTUP.read_text(encoding="utf-8")
    core = _between(startup, "static void app_startup_initialize_core", "static void app_startup_initialize_services")
    services = _between(startup, "static void app_startup_initialize_services", "void app_startup_run")

    assert core.index("seismic_buffer_init()") < core.index("seismic_acquisition_init()") < core.index("adc_service_init()")

    assert "seismic_acquisition_start()" not in services

    interactive = services.index('"interactive_ready"')
    web_start = services.index("web_server_start()", interactive)
    wifi_start = services.index("wifi_manager_start()", interactive)
    assert interactive < web_start < wifi_start


def test_measurements_screen_entry_starts_acquisition():
    registry = UI_SCREEN_REGISTRY.read_text(encoding="utf-8")
    open_measurements = _between(
        registry,
        "static void ui_screen_open_measurements(void)",
        "static void ui_screen_open_storage(void)",
    )
    assert "seismic_acquisition_start()" in open_measurements
    assert "ui_render_measurements_locked();" in open_measurements
    assert open_measurements.index("seismic_acquisition_start()") < open_measurements.index(
        "ui_render_measurements_locked();"
    )


def test_web_measurement_start_ensures_acquisition_before_queuing():
    source = WEB_CONTROL.read_text(encoding="utf-8")
    helper = _between(
        source,
        "static esp_err_t web_control_measurement_start",
        "static esp_err_t web_control_acquisition_stop",
    )
    assert helper.index("seismic_acquisition_start()") < helper.index(
        "measurement_service_start_object_async(object_id)"
    )


def test_web_stop_keeps_measurement_guard_before_serialized_acquisition_stop():
    source = WEB_CONTROL.read_text(encoding="utf-8")
    helper = _between(
        source,
        "static esp_err_t web_control_acquisition_stop",
        "static esp_err_t web_control_process",
    )

    assert helper.index("measurement_service_snapshot_blocks_acquisition_stop") < helper.index(
        "seismic_acquisition_stop()"
    )
