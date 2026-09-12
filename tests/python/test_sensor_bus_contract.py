"""Static contracts for the Phase E shared sensor I2C architecture."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_sensor_bus_owns_the_board_contract_and_vendor_bus():
    source = read("components/sensor_bus/sensor_bus.c")
    header = read("components/sensor_bus/include/sensor_bus.h")
    cmake = read("components/sensor_bus/CMakeLists.txt")
    manifest = read("components/sensor_bus/idf_component.yml")
    board = read("components/board_config/include/board_config.h")

    assert "i2c_bus_create(" in source
    assert "i2c_bus_device_create(" in source
    assert "i2c_bus_device_delete(" in source
    assert "i2c_bus_delete(" in source
    assert "I2C_NUM_0" in source
    assert ".master.clk_speed = SENSOR_BUS_I2C_SPEED_HZ" in source
    assert "SENSOR_BUS_I2C_SPEED_HZ  100000U" in header
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
    assert "i2c_bus_read_bytes(" in driver
    assert "i2c_bus_write_bytes(" in driver
    assert "i2c_new_master_bus(" not in driver
    assert "i2c_del_master_bus(" not in driver
    assert "i2c_master_bus_add_device(" not in driver
    assert "i2c_master_bus_rm_device(" not in driver
    assert "i2c_bus_handle_t" in header
    assert "i2c_bus_device_handle_t" in header
    assert "sensor_bus" in cmake
    assert "esp_driver_i2c" not in cmake


def test_sensor_hub_is_deferred_until_a_real_mux_contract_exists():
    adr = read("docs/adr-001-phase-e-i2c-bus.md")

    assert "sensor_hub" in adr
    assert "TCA9548A" in adr
    assert "no" in adr and "TCA9548A address" in adr
    assert "There is no `sensor_hub` component" in adr
