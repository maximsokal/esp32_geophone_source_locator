import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = REPO_ROOT / "nRF54L15" / "platformio.ini"
NRF_TEST_ROOT = REPO_ROOT / "nRF54L15" / "tests"
PLATFORMIO_TEST_ROOT = NRF_TEST_ROOT / "platformio_native"
NCS_TEST_ROOT = NRF_TEST_ROOT / "ncs_native"
NCS = REPO_ROOT / "nRF54L15" / "ncs.ps1"
README = REPO_ROOT / "nRF54L15" / "README.md"
TEST_TASK = REPO_ROOT / "docs" / "nrf54l15-test-task.md"
CMAKE = REPO_ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt"
PRJ_CONF = REPO_ROOT / "nRF54L15" / "zephyr" / "prj.conf"
KCONFIG = REPO_ROOT / "nRF54L15" / "zephyr" / "Kconfig"
PLATFORMIO_CONF = REPO_ROOT / "nRF54L15" / "zephyr" / "prj.platformio.conf"
APP_OVERLAY = REPO_ROOT / "nRF54L15" / "zephyr" / "app.overlay"
HOST_NRF_MANIFEST = REPO_ROOT / "tests" / "host" / "manifest.d" / "coverage-nrf.json"
LOGGER_LED_SOURCE = REPO_ROOT / "nRF54L15" / "src" / "nrf_logger_led.c"
LOGGER_LED_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_logger_led_zephyr.c"
SENSOR_RUNTIME_ZEPHYR = (
    REPO_ROOT / "nRF54L15" / "src" / "nrf_sensor_upstream_runtime_zephyr.c"
)
LOG_RUNTIME_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_log_runtime_zephyr.c"
MAIN = REPO_ROOT / "nRF54L15" / "src" / "main.c"
SENSOR_BUS_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_sensor_bus_zephyr.c"
BMI160_HEADER = REPO_ROOT / "nRF54L15" / "include" / "nrf_bmi160.h"
BMI160_CORE = REPO_ROOT / "nRF54L15" / "src" / "nrf_bmi160.c"
BMI160_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_bmi160_zephyr.c"
BME280_HEADER = REPO_ROOT / "nRF54L15" / "include" / "nrf_bme280.h"
BME280_CORE = REPO_ROOT / "nRF54L15" / "src" / "nrf_bme280.c"
BME280_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_bme280_zephyr.c"
REMOTE_RUNTIME = REPO_ROOT / "nRF54L15" / "src" / "nrf_remote_sensor_runtime.c"
REMOTE_RUNTIME_ZEPHYR = (
    REPO_ROOT / "nRF54L15" / "src" / "nrf_remote_sensor_runtime_zephyr.c"
)
VERIFIED_SEEED_PLATFORM = "1ec1287f8e4bc4067a6fd593991e36875aef989f"


def test_platformio_compatibility_and_native_test_environments_are_explicit():
    source = PLATFORMIO.read_text(encoding="utf-8")

    environments = re.findall(r"(?m)^\[env:([^\]]+)\]$", source)
    assert environments == ["seeed-xiao-nrf54l15", "native"]
    assert (
        "platform = https://github.com/Seeed-Studio/platform-seeedboards.git#"
        + VERIFIED_SEEED_PLATFORM
    ) in source
    assert "framework = zephyr" in source
    assert "board = seeed-xiao-nrf54l15" in source
    assert "sense" not in source.lower()
    assert "[env:native]" in source
    assert "test_framework = unity" in source
    assert "test_dir = tests/platformio_native" in source

    assert PLATFORMIO_TEST_ROOT.is_dir()
    assert (PLATFORMIO_TEST_ROOT / "test_sw_upstream_protocol").is_dir()
    assert NCS_TEST_ROOT.is_dir()
    assert not (REPO_ROOT / "nRF54L15" / "test").exists()

    wrapper = NCS.read_text(encoding="utf-8")
    assert '$NcsVersion = "v3.4.0"' in wrapper
    assert '"nrf54l15-production-v3.4.0"' in wrapper


def test_nrf_zephyr_target_compiles_runtime_policy_and_sensor_pipeline_sources():
    source = CMAKE.read_text(encoding="utf-8")

    assert 'set(EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay")' in source
    assert "../src/main.c" in source
    assert "../src/nrf_node_state.c" in source
    assert "../src/nrf_sensor_bus.c" in source
    assert "../src/nrf_sensor_bus_zephyr.c" in source
    assert "../src/nrf_bmi160.c" in source
    assert "../src/nrf_bmi160_zephyr.c" in source
    assert "../src/nrf_bme280.c" in source
    assert "../src/nrf_bme280_zephyr.c" in source
    assert "../src/nrf_sensor_snapshot.c" in source
    assert "../src/nrf_remote_sensor_runtime.c" in source
    assert "../src/nrf_remote_sensor_runtime_zephyr.c" in source
    assert "../src/nrf_cs_scheduler.c" in source
    assert "target_include_directories(app PRIVATE ../include)" in source


def test_nrf_logger_led_policy_is_host_covered_and_zephyr_neutral():
    cmake = CMAKE.read_text(encoding="utf-8")
    source = LOGGER_LED_SOURCE.read_text(encoding="utf-8")
    manifest = json.loads(HOST_NRF_MANIFEST.read_text(encoding="utf-8"))
    suite = next(test for test in manifest["tests"] if test["name"] == "nrf_logger_led")

    assert "../src/nrf_logger_led.c" in cmake
    assert "../src/nrf_logger_led_zephyr.c" in cmake
    assert "nRF54L15/src/nrf_logger_led.c" in suite["sources"]
    assert "tests/host/nrf_logger_led_test.c" in suite["sources"]
    assert not re.search(r'(?m)^\s*#\s*include\s*[<"][^>"]*zephyr/', source)

    runtime = SENSOR_RUNTIME_ZEPHYR.read_text(encoding="utf-8")
    assert re.search(
        r"state\.bmi160_init_failed\s*=\s*!s_runtime\.core\.imu_ready\s*"
        r"&&\s*s_runtime\.core\.imu_init_result\s*!=\s*0",
        runtime,
    )
    assert re.search(
        r"state\.bme280_init_failed\s*=\s*!s_runtime\.core\.env_ready\s*"
        r"&&\s*s_runtime\.core\.env_init_result\s*!=\s*0",
        runtime,
    )
    assert "input->bmi160_init_failed = state->bmi160_init_failed;" in source
    assert "input->bme280_init_failed = state->bme280_init_failed;" in source
    assert re.search(
        r"if\s*\(state->bme280_ready\s*&&\s*"
        r"\(state->sensor_fault_mask\s*&\s*NRF_SENSOR_VALID_ENV\)\s*!=\s*0U\)",
        source,
    )
    assert "in->bme280_init_failed" in source
    assert "!in->bme280_ready" not in source
    assert "!state->bme280_ready" not in source


def test_logger_led_dt_backend_is_optional_and_main_hook_stays_small():
    overlay = APP_OVERLAY.read_text(encoding="utf-8")
    glue = LOGGER_LED_ZEPHYR.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    runtime = SENSOR_RUNTIME_ZEPHYR.read_text(encoding="utf-8")
    log_runtime = LOG_RUNTIME_ZEPHYR.read_text(encoding="utf-8")
    readme = README.read_text(encoding="utf-8")

    aliases = re.findall(r"(?m)^\s*(logger-(?:green|yellow|red))\s*=", overlay)
    assert sorted(aliases) == ["logger-green", "logger-red", "logger-yellow"]
    for color, pin in (("green", 5), ("yellow", 6), ("red", 7)):
        assert re.search(
            rf"logger_{color}:.*?gpios\s*=\s*<&gpio1\s+{pin}\s+GPIO_ACTIVE_HIGH>",
            overlay,
            re.DOTALL,
        )
        assert glue.count(
            f"GPIO_DT_SPEC_GET_OR(DT_ALIAS(logger_{color}), gpios, {{0}})"
        ) == 1

    polarity_note = (
        r"ASSUMED active-high, same as BLUE; NOT verified \(MAX-43\); "
        r"verify with a current-limited test before trusting\."
    )
    overlay_comment = re.sub(r"(?m)^\s*\*\s?", "", overlay)
    assert re.search(polarity_note, " ".join(overlay_comment.split()))
    assert re.search(polarity_note, " ".join(readme.split()))
    assert "GPIO_OUTPUT_INACTIVE" in glue
    assert "gpio_pin_set_dt" in glue
    assert "gpio_dt_spec" in glue
    assert not re.search(
        r"i2c_(?:read|write|transfer)|nrf_(?:sensor_bus|bmi160|bme280)",
        glue,
    )

    assert main.count("nrf_logger_led_zephyr_init()") == 1
    assert main.count("nrf_logger_led_zephyr_latch_init_fatal()") == 1
    assert "nrf_logger_led_update(" not in main
    assert runtime.count("nrf_logger_led_update(") == 1
    assert runtime.count("nrf_logger_led_reset(") == 1
    assert "nrf_log_runtime_zephyr_get_led_counters(&counters)" in runtime
    for event in (
        "TWIS_INVALID_FRAME",
        "TWIS_UNKNOWN_TYPE",
        "TWIS_BUSY",
        "CS_EXECUTE_FAILED",
        "CS_TIMEOUT",
        "WATCHDOG_PARTICIPANT_STALE",
        "ROLE_LOST",
    ):
        assert f"NRF_LOG_EVENT_{event}" in log_runtime


def test_platformio_uses_a_minimal_compatibility_config_without_changing_ncs_production():
    cmake = CMAKE.read_text(encoding="utf-8")
    compatibility = PLATFORMIO_CONF.read_text(encoding="utf-8")
    production = PRJ_CONF.read_text(encoding="utf-8")

    assert "if(DEFINED PIO_PACKAGES_DIR)" in cmake
    assert 'set(CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/prj.platformio.conf")' in cmake
    assert "CONFIG_I2C_TARGET_BUFFER_MODE=y" in compatibility
    assert "CONFIG_I2C_NRFX_TWIS_BUF_SIZE=144" in compatibility
    assert "CONFIG_BT_CHANNEL_SOUNDING" not in compatibility
    assert "CONFIG_BT_LL_SOFTDEVICE" not in compatibility
    assert "CONFIG_BT_CHANNEL_SOUNDING=y" in production
    assert "CONFIG_BT_LL_SOFTDEVICE=y" in production


def test_plain_xiao_overlay_disables_phantom_sense_imu_and_exposes_upstream_twis():
    source = APP_OVERLAY.read_text(encoding="utf-8")

    assert "/delete-property/ imu0;" in source
    assert "&lsm6ds3tr_c" in source
    assert 'status = "disabled";' in source
    assert "&i2c30" not in source
    assert "&i2c22" in source
    assert 'compatible = "nordic,nrf-twis";' in source
    assert "NRF_PSEL(TWIS_SDA, 1, 10)" in source
    assert "NRF_PSEL(TWIS_SCL, 1, 11)" in source


def test_private_sensor_bus_uses_i2c30_only_and_i2c_is_explicitly_enabled():
    source = SENSOR_BUS_ZEPHYR.read_text(encoding="utf-8")
    config = PRJ_CONF.read_text(encoding="utf-8")

    assert "DT_NODELABEL(i2c30)" in source
    assert "DT_NODELABEL(i2c22)" not in source
    assert "i2c_write_read(" in source
    assert "i2c_write(" in source
    assert "CONFIG_I2C=y" in config


def test_bmi160_is_runtime_optional_instead_of_an_always_present_dt_device():
    overlay = APP_OVERLAY.read_text(encoding="utf-8")
    config = PRJ_CONF.read_text(encoding="utf-8")
    header = BMI160_HEADER.read_text(encoding="utf-8")
    core = BMI160_CORE.read_text(encoding="utf-8")
    bridge = BMI160_ZEPHYR.read_text(encoding="utf-8")

    assert "bosch,bmi160" not in overlay
    assert "CONFIG_BMI160" not in config
    assert "NRF_BMI160_DEFAULT_ADDRESS = 0x68U" in header
    assert "NRF_BMI160_CHIP_ID = 0xD1U" in header
    assert "BMI160_CMD_SOFT_RESET = 0xB6U" in core
    assert "BMI160_ACCEL_CONFIG_100HZ_NORMAL = 0x28U" in core
    assert "BMI160_ACCEL_RANGE_2G = 0x03U" in core
    assert "BMI160_GYRO_CONFIG_100HZ_NORMAL = 0x28U" in core
    assert "BMI160_GYRO_RANGE_250DPS = 0x03U" in core
    assert "nrf_sensor_bus_zephyr_init_private(&bus)" in bridge
    assert "nrf_bmi160_init_observed(" in bridge


def test_bme280_is_runtime_optional_and_uses_bounded_forced_measurements():
    overlay = APP_OVERLAY.read_text(encoding="utf-8")
    config = PRJ_CONF.read_text(encoding="utf-8")
    header = BME280_HEADER.read_text(encoding="utf-8")
    core = BME280_CORE.read_text(encoding="utf-8")
    bridge = BME280_ZEPHYR.read_text(encoding="utf-8")

    assert "bosch,bme280" not in overlay
    assert "CONFIG_BME280" not in config
    assert "NRF_BME280_ADDRESS_PRIMARY = 0x76U" in header
    assert "NRF_BME280_ADDRESS_SECONDARY = 0x77U" in header
    assert "NRF_BME280_CHIP_ID = 0x60U" in header
    assert "BME280_SOFT_RESET = 0xB6U" in core
    assert "BME280_CTRL_HUM_X1 = 0x01U" in core
    assert "BME280_CTRL_MEAS_SLEEP = 0x54U" in core
    assert "BME280_CTRL_MEAS_FORCED = 0x55U" in core
    assert "BME280_CONFIG_FILTER_X16 = 0x10U" in core
    assert "BME280_NVM_POLL_ATTEMPTS = 10U" in core
    assert "BME280_MEASUREMENT_POLL_ATTEMPTS = 5U" in core
    assert "nrf_sensor_bus_zephyr_init_private(&bus)" in bridge
    assert "nrf_bme280_init_observed(" in bridge
    assert "nrf_bme280_result_t nrf_bme280_zephyr_init_remote" in bridge


def test_remote_sensor_runtime_wires_real_drivers_only_after_remote_role():
    core = REMOTE_RUNTIME.read_text(encoding="utf-8")
    bridge = REMOTE_RUNTIME_ZEPHYR.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")

    assert "node_state->lifecycle != NRF_NODE_LIFECYCLE_READY" in core
    assert "node_state->role != NRF_NODE_ROLE_REMOTE" in core
    assert "runtime->imu_ready" in core
    assert "runtime->env_ready" in core
    assert "nrf_sensor_snapshot_refresh(" in core

    assert "nrf_bmi160_zephyr_init_remote(device)" in bridge
    assert "nrf_bme280_zephyr_init_remote(device)" in bridge
    assert ".read_imu = nrf_bmi160_read_imu" in bridge
    assert ".read_env = nrf_bme280_read_env" in bridge

    assert "nrf_remote_sensor_runtime_zephyr_prepare(" not in main
    assert "nrf_remote_sensor_runtime_zephyr_refresh(" not in main


def test_nrf_bringup_enters_ready_unassigned_runtime_state_without_forcing_role():
    source = MAIN.read_text(encoding="utf-8")

    assert '#include "nrf_node_state.h"' in source
    assert "nrf_node_state_init(&node_state);" in source
    assert "nrf_node_state_mark_ready(&node_state)" in source
    assert "nrf_node_state_assign_role(" not in source
    assert "nrf_bmi160_zephyr_init_remote(" not in source
    assert "nrf_bme280_zephyr_init_remote(" not in source


def test_nrf_docs_record_ncs_as_canonical_and_old_seeed_revision_as_compatibility():
    task = TEST_TASK.read_text(encoding="utf-8")
    readme = README.read_text(encoding="utf-8")

    assert VERIFIED_SEEED_PLATFORM in task
    assert "compatibility" in task.lower()
    assert "nRF Connect SDK v3.4.0" in task
    assert "canonical" in readme.lower()
    assert "plain Seeed Studio XIAO nRF54L15" in readme
    assert re.search(r"not\s+the\s+XIAO\s+nRF54L15\s+Sense", readme)


def test_nrf_automatic_test_contract_does_not_equate_no_upload_with_native_execution():
    task = TEST_TASK.read_text(encoding="utf-8")

    assert "no-upload mode does not replace NCS evidence" in task
    assert "considered physically verified" in task
    assert "NCS-native" in task


def test_nrf_boot_diagnostic_kconfig_defaults_are_enabled_and_rate_limited():
    kconfig = KCONFIG.read_text(encoding="utf-8")
    production = PRJ_CONF.read_text(encoding="utf-8")

    assert kconfig.startswith('source "Kconfig.zephyr"\n\nmenu "Application options"')
    assert kconfig.rstrip().endswith("endmenu")
    assert re.search(
        r"(?ms)^config APP_LED_BOOT_LAMP_TEST\s+bool[^\n]*\s+default y\b",
        kconfig,
    )
    assert re.search(
        r"(?ms)^config APP_NRF_STATUS_PRINT_PERIOD_MS\s+int[^\n]*"
        r"\s+range 0 3600000\s+default 5000\b",
        kconfig,
    )
    assert re.search(
        r"(?ms)^config APP_NRF_VERBOSE_LOG\s+bool[^\n]*\s+default y\b",
        kconfig,
    )
    assert re.search(
        r"(?ms)^config APP_LOG_TEST_EVENTS\s+bool[^\n]*\s+default n\b",
        kconfig,
    )
    assert "CONFIG_APP_LED_BOOT_LAMP_TEST=y" in production
    assert "CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS=5000" in production
    assert "CONFIG_APP_NRF_VERBOSE_LOG=y" in production
