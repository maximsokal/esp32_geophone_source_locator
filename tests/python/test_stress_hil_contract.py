from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_MAIN = REPO_ROOT / "tests" / "target" / "stress_app" / "main" / "stress_app_main.c"
TARGET_PHYSICAL = REPO_ROOT / "tests" / "target" / "stress_app" / "main" / "stress_physical_probe.c"
TARGET_CMAKE = REPO_ROOT / "tests" / "target" / "stress_app" / "main" / "CMakeLists.txt"
TARGET_DEFAULTS = REPO_ROOT / "tests" / "target" / "stress_app" / "sdkconfig.defaults"
HIL_RUNNER = REPO_ROOT / "tools" / "run_stress_hil.ps1"
HOST_STRESS_RUNNER = REPO_ROOT / "tools" / "run_stress_tests.ps1"
VIRTUAL_BOARD_RUNNER = REPO_ROOT / "tools" / "run_virtual_board_stress.ps1"
IDF_RUNNER = REPO_ROOT / "tools" / "run_idf.ps1"


def test_target_stress_app_has_real_scheduler_heap_and_sweep_contracts():
    source = TARGET_MAIN.read_text(encoding="utf-8")
    cmake = TARGET_CMAKE.read_text(encoding="utf-8")
    defaults = TARGET_DEFAULTS.read_text(encoding="utf-8")

    for marker in (
        "xTaskCreatePinnedToCore",
        "uxTaskGetStackHighWaterMark",
        "heap_caps_get_free_size",
        "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA",
        "MALLOC_CAP_SPIRAM",
        "ESP_TIMER_ISR",
        "phase->isr_ticks >= expected_min",
        "TARGET_STRESS_BINARY_SEARCH",
        "TARGET_STRESS_TRACE",
        "sensor_random_state",
        "network_random_state",
        "scheduler_random_state",
        "recorder_batch",
        "const bool expected_saturated = rates[index] == STRESS_RATE_MAX_HZ",
    ):
        assert marker in source
    assert "seismic_frame_t batch[" not in source
    assert "components/seismic_buffer/tests/stubs" not in cmake
    assert "esp_psram" in cmake
    for marker in (
        "CONFIG_SPIRAM=y",
        "CONFIG_SPIRAM_MODE_OCT=y",
        "CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD=y",
    ):
        assert marker in defaults

    physical = TARGET_PHYSICAL.read_text(encoding="utf-8")
    for marker in (
        "ad7606_driver_presence_monitor_start",
        "AD7606_SERIAL_PATTERN_VARIED",
        "STRESS_PHYSICAL_AD7606_SAMPLES",
        "STRESS_PHYSICAL_SD_TESTS",
        "STRESS_PHYSICAL_TFT_DRAWS",
        "STRESS_PHYSICAL_I2C_READS",
        "STRESS_PHYSICAL_WIFI_SCANS",
        "sd_card_diagnostics_run_write_read_test",
        "esp_lcd_panel_draw_bitmap",
        "i2c_bus_read_byte",
        "esp_wifi_scan_start",
        "TARGET_STRESS_PHYSICAL",
    ):
        assert marker in physical


def test_stress_hil_runner_separates_replay_target_and_physical_evidence():
    runner = HIL_RUNNER.read_text(encoding="utf-8")

    for marker in (
        "SkipBuild",
        "WaitForPowerHandoff",
        "POWER_HANDOFF_REQUIRED",
        "preflightPorts",
        "unplug the same USB cable used for COM4",
        "-RequirePhysical requires -WaitForPowerHandoff",
        "Stop-ProcessTree",
        "ParentProcessId",
        "Target stress identity",
        "RequirePhysical",
        "ConfirmAd7606",
        "ConfirmSpi2Sd",
        "ConfirmTft",
        "ConfirmWifi",
        "ConfirmI2c",
        "TARGET_STRESS_SUMMARY",
        "TARGET_STRESS_BINARY_SEARCH",
        "TARGET_STRESS_SCHEDULE",
        "PHYSICAL_CONDITION: NOT_CONFIRMED",
        "TARGET_STRESS_HIL_RESULT",
        "inversions",
        "replay=(PASS|FAIL)",
        "[UInt64]$schedule.Groups[1].Value -ge 64",
        "[UInt64]$schedule.Groups[2].Value -gt 0",
        "Groups[3].Value -eq \"PASS\"",
        "[UInt64]$isr.Groups[1].Value -ge [UInt64]$isr.Groups[2].Value",
        "TARGET_STRESS_PHYSICAL",
        "probePattern",
        "Driver probe passed",
        "expectedSweepRates",
        "5000 Hz is the expected saturation boundary control",
        "STRESS_RUN_COMPLETE",
        "BROWNOUT_ABORT",
        "Target power/brownout markers",
    ):
        assert marker in runner
    assert "saved UART log into current physical proof" in runner


def test_idf_monitor_forces_utf8_for_redirected_uart_logs():
    runner = IDF_RUNNER.read_text(encoding="utf-8")
    assert '$env:PYTHONUTF8 = "1"' in runner
    assert '$env:PYTHONIOENCODING = "utf-8"' in runner


def test_host_stress_runner_is_a_thin_alias_for_the_virtual_board_runner():
    runner = HOST_STRESS_RUNNER.read_text(encoding="utf-8")
    assert "Compatibility alias" in runner
    assert "run_virtual_board_stress.ps1" in runner
    assert '"--only"' not in runner


def test_virtual_board_runner_is_software_only():
    runner = VIRTUAL_BOARD_RUNNER.read_text(encoding="utf-8")
    for marker in (
        "VIRTUAL_BOARD_RUNNER: SOFTWARE_ONLY",
        "VIRTUAL_BOARD_PHYSICAL_IO: NONE",
        "VIRTUAL_BOARD_TARGET_FLASH: NONE",
        "VIRTUAL_BOARD_SERIAL_PORTS: NONE",
        '"--only", "virtual_board_stress"',
        '"--only", "scheduler_replay_stress"',
        "VIRTUAL_BOARD_STRESS_RUNNER_RESULT",
    ):
        assert marker in runner
    assert "run_idf.ps1" not in runner
    assert "-Port" not in runner


def test_virtual_board_stress_models_all_diploma_buses():
    source = (REPO_ROOT / "tests" / "host" / "performance_stress_test.c").read_text(encoding="utf-8")
    for marker in (
        "virtual_board_t",
        "virtual_ad7606_service",
        "virtual_wifi_service",
        "virtual_i2c_service",
        "virtual_ad7606_varied_payloads",
        "virtual_wifi_events",
        "virtual_i2c_transactions",
        "VIRTUAL_BOARD_STRESS_RESULT",
        "physical_io=NONE",
    ):
        assert marker in source
