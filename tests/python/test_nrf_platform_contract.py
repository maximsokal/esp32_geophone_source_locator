from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = REPO_ROOT / "nRF54L15" / "platformio.ini"
NCS = REPO_ROOT / "nRF54L15" / "ncs.ps1"
README = REPO_ROOT / "nRF54L15" / "README.md"
TEST_TASK = REPO_ROOT / "docs" / "nrf54l15-test-task.md"
CMAKE = REPO_ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt"
PRJ_CONF = REPO_ROOT / "nRF54L15" / "zephyr" / "prj.conf"
PLATFORMIO_CONF = REPO_ROOT / "nRF54L15" / "zephyr" / "prj.platformio.conf"
APP_OVERLAY = REPO_ROOT / "nRF54L15" / "zephyr" / "app.overlay"
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


def test_platformio_compatibility_environment_remains_commit_pinned_and_single():
    source = PLATFORMIO.read_text(encoding="utf-8")

    environments = re.findall(r"(?m)^\[env:([^\]]+)\]$", source)
    assert environments == ["seeed-xiao-nrf54l15"]
    assert (
        "platform = https://github.com/Seeed-Studio/platform-seeedboards.git#"
        + VERIFIED_SEEED_PLATFORM
    ) in source
    assert "framework = zephyr" in source
    assert "board = seeed-xiao-nrf54l15" in source
    assert "sense" not in source.lower()

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


def test_platformio_uses_a_minimal_compatibility_config_without_changing_ncs_production():
    cmake = CMAKE.read_text(encoding="utf-8")
    compatibility = PLATFORMIO_CONF.read_text(encoding="utf-8")
    production = PRJ_CONF.read_text(encoding="utf-8")

    assert "if(DEFINED PIO_PACKAGES_DIR)" in cmake
    assert 'set(CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/prj.platformio.conf")' in cmake
    assert "CONFIG_I2C_TARGET_BUFFER_MODE=y" in compatibility
    assert "CONFIG_I2C_NRFX_TWIS_BUF_SIZE=128" in compatibility
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
    assert "nrf_bmi160_init(" in bridge


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
    assert "nrf_bme280_init(" in bridge
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

    assert "`pio test --without-uploading`" in task
    assert "does not prove a test is physically hardware-free" in task
    assert "NCS-native" in task
