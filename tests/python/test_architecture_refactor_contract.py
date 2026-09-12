"""Static contracts for the official ESP-IDF infrastructure migration."""

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_requested_managed_components_are_exactly_pinned_and_qrcode_is_rejected():
    manifest = read("main/idf_component.yml")
    lock = read("dependencies.lock")

    versions = {
        "espressif/button": "4.2.0",
        "espressif/esp_insights": "1.3.4",
        "espressif/esp-dsp": "1.8.2",
        "espressif/led_indicator": "2.1.2",
        "espressif/i2c_bus": "1.5.2",
    }
    for name, version in versions.items():
        assert f'{name}: "{version}"' in manifest
        pattern = rf"(?ms)^  {re.escape(name)}:\n.*?^    version: {re.escape(version)}$"
        assert re.search(pattern, lock), f"{name} is not locked at {version}"

    assert "espressif/qrcode" not in manifest
    assert "espressif/qrcode" not in lock
    adr = read("docs/ADR/0001-qr-rendering.md")
    assert "lv_qrcode" in adr
    assert "Do not add `espressif/qrcode`" in adr


def test_old_application_owned_input_and_event_files_are_removed():
    removed = (
        "components/app_event_bus",
        "components/keypad/keypad_adc.c",
        "components/keypad/keypad_decode.c",
        "components/keypad/keypad_event_monitor.c",
        "components/keypad/keypad_event_worker.c",
        "components/power_control/power_button_monitor.c",
        "components/power_control/power_button_monitor_task.c",
        "components/ui/ui_event_task.c",
    )
    for relative_path in removed:
        path = REPO_ROOT / relative_path
        if path.is_dir():
            assert not any(child.is_file() for child in path.rglob("*")), relative_path
        else:
            assert not path.exists(), relative_path

    task_config = read("components/app_config/include/app_task_config.h")
    assert "APP_TASK_PRIORITY_APP_EVENT_LOOP" in task_config
    assert "APP_TASK_STACK_APP_EVENT_LOOP  6144" in task_config
    assert "APP_TASK_STACK_KEYPAD" not in task_config
    assert "APP_TASK_STACK_POWER_BUTTON" not in task_config


def test_status_insights_and_dsp_boundaries_are_explicit():
    status = read("components/status_indicator/status_indicator.c")
    status_header = read("components/status_indicator/include/status_indicator.h")
    insights = read("components/insights_bridge/insights_bridge.c")
    startup = read("main/app_startup.c")
    dsp = read("components/seismic_dsp/seismic_dsp.c")
    dsp_header = read("components/seismic_dsp/include/seismic_dsp.h")

    assert "led_indicator.h" in status
    assert "return false;" in status[status.index("status_indicator_backend_enabled") :]
    assert "APP_STATUS_WARNING" in status_header
    assert "APP_STATUS_ERROR" in status_header
    assert "APP_STATUS_CLEAR_NON_STICKY" in status_header
    assert "s_warning_sticky" in status
    assert "s_error_sticky" in status

    assert "CONFIG_APP_ESP_INSIGHTS" in insights
    assert "esp_insights_init" in insights
    assert '"internal_free"' in insights
    assert '"acquisition_heartbeat"' in insights
    assert ".log_type = 0U" in insights
    assert startup.index("wifi_manager_init()") < startup.index("insights_bridge_init()")
    assert startup.index("insights_bridge_init()") < startup.index("wifi_manager_start()")
    assert "esp_event_handler_register_with" in insights
    assert "app_events_get_loop()" in insights
    assert "APP_EVENT_WIFI_STA_CONNECTED" in insights
    assert "s_retry_pending" in insights
    assert "CONFIG_DIAG_ENABLE_VARIABLES=n" in insights

    assert "seismic_dsp_init(" in dsp
    assert "seismic_dsp_process_block(" in dsp
    assert "SEISMIC_DSP_STAGE_DETREND" in dsp_header
    assert "SEISMIC_DSP_STAGE_FIR" in dsp_header
    assert "SEISMIC_DSP_STAGE_IIR" in dsp_header
    assert "SEISMIC_DSP_STAGE_CROSS_CORRELATION" in dsp_header
    assert "xTaskCreate" not in dsp
    assert "input_passthrough = true" in dsp


def test_insights_profile_is_explicit_and_uses_non_secret_auth_fixture():
    defaults = read("sdkconfig.insights.defaults")
    root_cmake = read("CMakeLists.txt")
    component_cmake = read("components/insights_bridge/CMakeLists.txt")
    kconfig = read("components/insights_bridge/Kconfig")
    provider = read("components/insights_bridge/insights_test_auth_provider.c")
    idf_runner = read("tools/run_idf.ps1")
    profile_runner = read("tools/run_insights_profile.ps1")

    assert "CONFIG_APP_ESP_INSIGHTS=y" in defaults
    assert "CONFIG_APP_ESP_INSIGHTS_TEST_AUTH_PROVIDER=y" in defaults
    assert "CONFIG_DIAG_ENABLE_METRICS=y" in defaults
    assert "# CONFIG_DIAG_ENABLE_HEAP_METRICS is not set" in defaults
    assert "# CONFIG_DIAG_ENABLE_WIFI_METRICS is not set" in defaults
    assert "# CONFIG_DIAG_ENABLE_VARIABLES is not set" in defaults
    assert "CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS=y" in defaults
    assert "GEOLOCATOR_INSIGHTS_PROFILE" in root_cmake
    assert "[string]$Sdkconfig" in idf_runner
    assert '"SDKCONFIG=$sdkconfigPath"' in idf_runner
    assert "-Sdkconfig $profileSdkconfig" in profile_runner
    assert "CONFIG_APP_ESP_INSIGHTS_TEST_AUTH_PROVIDER" in component_cmake
    assert "APP_ESP_INSIGHTS_TEST_AUTH_PROVIDER" in kconfig
    assert "geolocator-test-auth-key" in provider
    assert "espressif/qrcode" not in defaults + root_cmake + component_cmake


def test_shared_bus_and_ui_callback_have_nonblocking_ownership_boundaries():
    sensor_bus = read("components/sensor_bus/sensor_bus.c")
    ina = read("components/ina3221_monitor/ina3221_driver.c")
    ui_handler = read("components/ui/ui_event_dispatcher.c")

    assert sensor_bus.count("i2c_bus_create(") == 1
    assert sensor_bus.count("i2c_bus_delete(") >= 1
    assert "I2C_NUM_0" in sensor_bus
    assert "i2c_new_master_bus(" not in ina
    assert "i2c_del_master_bus(" not in ina

    handler = ui_handler[ui_handler.index("static void ui_event_handler") :]
    handler = handler[: handler.index("void ui_event_dispatcher_dispatch_pending_locked")]
    assert "ui_event_pending_push(" in handler
    assert "lvgl_port_task_wake(" in handler
    assert "lvgl_port_lock(" not in handler
    assert "vTaskDelay(" not in handler


def test_refactor_baseline_and_architecture_decisions_are_documented():
    baseline = read("docs/architecture-refactor-baseline-2026-09-12.md")
    architecture = read("ARCHITECTURE.md")

    assert "0777d74df532184b405a02b3e9fccdb89baec49f" in baseline
    assert "NOT CAPTURED" in baseline
    assert "0x1fac80" in baseline
    assert "0x1ff640" in baseline
    assert "APP_EVENT" in architecture
    assert "sensor_bus" in architecture
    assert "official Button" in architecture
