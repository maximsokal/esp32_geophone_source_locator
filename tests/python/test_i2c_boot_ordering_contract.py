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

    display = services.index("display_init()")
    boot_screen = services.index("ui_boot_init()")
    storage = services.index("storage_manager_init()")
    ina3221 = services.index("ina3221_monitor_start()")
    wifi_init = services.index("wifi_manager_init()")
    wifi_start = services.index("wifi_manager_start()")

    assert display < boot_screen < storage < ina3221 < wifi_init < wifi_start


def test_ina3221_monitor_remains_non_fatal_at_its_new_call_site():
    startup = read("main/app_startup.c")
    services = _function_body(
        startup, "static void app_startup_initialize_services("
    )
    call = services.index("ina3221_monitor_start()")
    preceding = services[max(0, call - 60) : call]
    assert "ESP_ERROR_CHECK(" not in preceding
    assert "INA3221 monitor unavailable" in services[call : call + 400]
