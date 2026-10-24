from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace_open = source.index("{", start)
    depth = 0
    i = brace_open
    while True:
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace_open : i + 1]
        i += 1


def test_shared_i2c_sensor_bus_starts_after_keypad_display_and_sd_stack():
    """The XIAO nRF54L15 main-enclosure node is wired as an upstream I2C
    target on the same physical bus INA3221/sensor_bus owns. Opening that
    bus before the local keypad/display/SD stack is interactive risks
    stalling boot on a bus a still-booting nRF node can be holding low."""
    startup = read("main/app_startup.c")

    core = _function_body(startup, "static void app_startup_initialize_core(void)")
    assert "ina3221_monitor_start()" not in core
    assert "keypad_init()" in core

    services = _function_body(
        startup, "static void app_startup_initialize_services("
    )

    display = services.index("app_storage_boot_ui_ensure_display_initialized()")
    boot_screen = services.index("app_storage_boot_ui_ensure_initialized()")
    storage = services.index("storage_manager_init()")
    ina3221 = services.index("ina3221_monitor_start()")
    wifi_init = services.index("wifi_manager_init()")
    wifi_start = services.index("wifi_manager_start()")

    assert display < boot_screen < storage < wifi_init < wifi_start < ina3221

    boot_ui = read("main/app_storage_boot_ui.c")
    display_helper = _function_body(
        boot_ui, "esp_err_t app_storage_boot_ui_ensure_display_initialized(void)"
    )
    ui_helper = _function_body(
        boot_ui, "esp_err_t app_storage_boot_ui_ensure_initialized(void)"
    )
    assert "display_init()" in display_helper
    assert "app_storage_boot_ui_ensure_display_initialized()" in ui_helper
    assert "ui_boot_init()" in ui_helper


def test_ina3221_monitor_remains_non_fatal_at_its_new_call_site():
    startup = read("main/app_startup.c")
    services = _function_body(
        startup, "static void app_startup_initialize_services("
    )
    call = services.index("ina3221_monitor_start()")
    preceding = services[max(0, call - 60) : call]
    assert "ESP_ERROR_CHECK(" not in preceding
    assert "INA3221 monitor unavailable" in services[call : call + 400]
    assert services.index("wifi_manager_start()") < call
    # The preflight probe (non-fatal, never ESP_ERROR_CHECK) sits between the
    # monitor start and its timing log, so the window covers both.
    timing = services.index("sensor_bus_i2c_start_after_wifi", call)
    assert timing - call < 1200
    between = services[call:timing]
    assert "ESP_ERROR_CHECK(" not in between


def test_wifi_completion_snapshot_and_marker_precede_i2c_worker_release():
    startup = read("main/app_startup.c")
    services = _function_body(startup, "static void app_startup_initialize_services(")
    wifi = services.index("wifi_manager_start()")
    snapshot = services.index('runtime_observability_log_snapshot("after-wifi")')
    monitor = services.index("ina3221_monitor_start()")
    assert wifi < snapshot < monitor
    marker = services.index("BOOT_PHASE wifi_start=complete i2c_start=allowed")
    assert snapshot < marker < monitor
