"""Static contracts for the Phase E shared sensor I2C architecture."""

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
RAW_I2C_BUS_CALL = re.compile(r"\bi2c_bus_[A-Za-z0-9_]+\s*\(")


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    body_start = source.index("{", start)
    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start:index + 1]
    raise AssertionError(f"unclosed function body: {signature}")


def test_sensor_bus_owns_the_board_contract_and_vendor_bus():
    source = read("components/sensor_bus/sensor_bus.c")
    header = read("components/sensor_bus/include/sensor_bus.h")
    cmake = read("components/sensor_bus/CMakeLists.txt")
    manifest = read("components/sensor_bus/idf_component.yml")
    board = read("components/board_config/include/board_config.h")

    assert "i2c_bus_create(" in source
    assert "i2c_bus_delete(" in source
    assert "i2c_bus_device_create(" not in source
    assert "i2c_bus_device_delete(" not in source
    assert "i2c_master_bus_add_device(" in source
    assert "i2c_master_bus_rm_device(" in source
    assert "I2C_NUM_0" in source
    assert ".master.clk_speed = SENSOR_BUS_I2C_SPEED_HZ" in source
    assert "SENSOR_BUS_I2C_SPEED_HZ  100000U" in header
    assert "SENSOR_BUS_TRANSACTION_TIMEOUT_MS 20U" in header
    assert "SENSOR_BUS_MUTEX_WAIT_MS 200U" in header
    assert "SENSOR_BUS_OWNER_MAX_HOLD_MS 150U" in header
    assert "GPIO_PULLUP_ENABLE" in source
    assert "BOARD_SENSOR_I2C_SDA_GPIO" in source
    assert "BOARD_SENSOR_I2C_SCL_GPIO" in source
    assert "BOARD_INA3221_I2C_ADDRESS" in source
    assert "i2c_bus" in cmake
    assert 'espressif/i2c_bus: "1.5.2"' in manifest
    assert "GPIO_NUM_9" in board
    assert "GPIO_NUM_10" in board
    assert "0x40" in board


def test_ina3221_borrows_handles_and_never_owns_the_shared_bus():
    driver = read("components/ina3221_monitor/ina3221_driver.c")
    header = read("components/ina3221_monitor/ina3221_driver.h")
    cmake = read("components/ina3221_monitor/CMakeLists.txt")

    assert "sensor_bus_get_handles(" in driver
    assert "sensor_bus_read_bytes(" in driver
    assert "sensor_bus_write_bytes(" in driver
    assert "i2c_bus_read_bytes(" not in driver
    assert "i2c_bus_write_bytes(" not in driver
    assert "i2c_new_master_bus(" not in driver
    assert "i2c_del_master_bus(" not in driver
    assert "i2c_master_bus_add_device(" not in driver
    assert "i2c_master_bus_rm_device(" not in driver
    assert "i2c_bus_handle_t" in header
    assert "i2c_bus_device_handle_t" in header
    assert "sensor_bus" in cmake
    assert "esp_driver_i2c" not in cmake


def test_sensor_bus_transfer_api_serializes_native_i2c_access():
    source = read("components/sensor_bus/sensor_bus.c")

    # The public read entry points delegate to one internal implementation, so
    # the lock/transfer contract lives there; both public variants must use it.
    read_wrapper = _function_body(source, "esp_err_t sensor_bus_read_bytes(")
    read_timeout_wrapper = _function_body(
        source, "esp_err_t sensor_bus_read_bytes_with_timeout("
    )
    read_body = _function_body(source, "static esp_err_t sensor_bus_read_bytes_internal(")
    write_body = _function_body(source, "esp_err_t sensor_bus_write_bytes(")

    assert "sensor_bus_read_bytes_internal(" in read_wrapper
    assert "sensor_bus_read_bytes_internal(" in read_timeout_wrapper
    for body in (read_body, write_body):
        assert "sensor_bus_lock_with_wait(" in body
        assert "sensor_bus_unlock();" in body
    assert "i2c_master_transmit_receive(" in read_body
    assert "i2c_master_receive(" in read_body
    assert "i2c_master_transmit(" in write_body


def test_raw_i2c_bus_calls_are_confined_to_sensor_bus_owner():
    production_roots = (
        REPO_ROOT / "components",
        REPO_ROOT / "main",
        REPO_ROOT / "nRF54L15" / "src",
    )
    offenders = []
    for root in production_roots:
        for source_path in root.rglob("*.c"):
            relative_parts = source_path.relative_to(REPO_ROOT).parts
            if "tests" in relative_parts:
                continue
            if relative_parts[:2] == ("components", "sensor_bus"):
                continue
            # The vendored ESP-IDF I2C driver override is the driver itself,
            # not a consumer of it (see components/esp_driver_i2c/NOTICE.md).
            if relative_parts[:2] == ("components", "esp_driver_i2c"):
                continue
            if RAW_I2C_BUS_CALL.search(source_path.read_text(encoding="utf-8")):
                offenders.append(source_path.relative_to(REPO_ROOT).as_posix())

    assert offenders == []


def test_sensor_hub_is_deferred_until_a_real_mux_contract_exists():
    adr = read("docs/adr-001-phase-e-i2c-bus.md")

    assert "sensor_hub" in adr
    assert "TCA9548A" in adr
    assert "no" in adr and "TCA9548A address" in adr
    assert "There is no `sensor_hub` component" in adr
