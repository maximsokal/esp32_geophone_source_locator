from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STARTUP = (ROOT / "main" / "app_startup.c").read_text(encoding="utf-8")
WIFI_STA = (ROOT / "components" / "wifi_manager" / "wifi_manager_sta.c").read_text(encoding="utf-8")


def test_boot_does_not_enable_diagnostic_ina3221_fast_sampling_before_wifi():
    """The diagnostic 50 Hz INA load must never be part of normal boot."""

    wifi_start = STARTUP.index("wifi_manager_start()")
    assert "ina3221_monitor_request_fast_sampling" not in STARTUP[:wifi_start]


def test_boot_does_not_drive_ad7606_gpio_before_the_adc_power_is_verified():
    """An independently powered ADC must not be driven during ESP32 boot."""

    assert "ad7606_driver_safe_gpio_init" not in STARTUP


def test_sta_does_not_force_continuous_radio_power_after_start():
    """The shared rail must retain the minimum modem power-save policy."""

    assert "esp_wifi_set_ps(\n            WIFI_PS_NONE" not in WIFI_STA
    assert "WIFI_PS_MIN_MODEM" in WIFI_STA


def test_runtime_ui_is_not_started_during_wifi_phy_bringup():
    """LVGL's periodic TFT work must not overlap esp_wifi_start()."""

    wifi_start = STARTUP.index("wifi_manager_start()")
    ui_start = STARTUP.index("ui_init()")
    keypad_start = STARTUP.index("keypad_start_event_monitor(")
    assert wifi_start < ui_start < keypad_start
    assert "dynamic LVGL timer" in STARTUP[ui_start - 500 : ui_start + 300]


def test_web_runtime_tasks_are_not_created_before_wifi_phy_bringup():
    """Web cache/lifecycle tasks must not add load before esp_wifi_start()."""

    wifi_start = STARTUP.index("wifi_manager_start()")
    web_start = STARTUP.index("web_server_start()")
    assert wifi_start < web_start


def test_wifi_start_has_explicit_phy_boundary_logs():
    """The next target log must distinguish entry from return of esp_wifi_start()."""

    assert '"STA PHY bring-up begin"' in WIFI_STA
    assert '"STA PHY bring-up complete"' in WIFI_STA
    assert WIFI_STA.index('"STA PHY bring-up begin"') < WIFI_STA.index("esp_wifi_start()")
    assert WIFI_STA.index("esp_wifi_start()") < WIFI_STA.index('"STA PHY bring-up complete"')
