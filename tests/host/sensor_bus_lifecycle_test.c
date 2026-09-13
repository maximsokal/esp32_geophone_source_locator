#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "sensor_bus.h"
#include "board_config.h"

static int s_bus_token;
static int s_device_token;
static bool s_fail_bus_create;
static bool s_fail_device_create;
static esp_err_t s_device_delete_result = ESP_OK;
static esp_err_t s_bus_delete_result = ESP_OK;
static unsigned s_bus_create_calls;
static unsigned s_device_create_calls;
static unsigned s_bus_delete_calls;
static unsigned s_device_delete_calls;
static i2c_port_t s_last_port;
static i2c_config_t s_last_config;
static uint8_t s_last_address;
static uint32_t s_last_device_speed;

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    ++s_bus_create_calls;
    s_last_port = port;
    s_last_config = *config;
    return s_fail_bus_create ? NULL : &s_bus_token;
}

i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t speed_hz)
{
    assert(bus == &s_bus_token);
    ++s_device_create_calls;
    s_last_address = address;
    s_last_device_speed = speed_hz;
    return s_fail_device_create ? NULL : &s_device_token;
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    ++s_device_delete_calls;
    assert(device != NULL && *device == &s_device_token);
    *device = NULL;
    return s_device_delete_result;
}

esp_err_t i2c_bus_delete(i2c_bus_handle_t *bus)
{
    ++s_bus_delete_calls;
    assert(bus != NULL && *bus == &s_bus_token);
    *bus = NULL;
    return s_bus_delete_result;
}

int main(void)
{
    sensor_bus_handles_t handles = {0};

    assert(sensor_bus_get_handles(NULL) == ESP_ERR_INVALID_ARG);

    s_fail_bus_create = true;
    assert(sensor_bus_init() == ESP_ERR_NO_MEM);
    assert(s_bus_create_calls == 1U);
    assert(s_device_create_calls == 0U);

    s_fail_bus_create = false;
    s_fail_device_create = true;
    assert(sensor_bus_init() == ESP_ERR_NO_MEM);
    assert(s_bus_create_calls == 2U);
    assert(s_device_create_calls == 1U);
    assert(s_bus_delete_calls == 1U);

    s_fail_device_create = false;
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    assert(handles.bus == &s_bus_token);
    assert(handles.ina3221 == &s_device_token);
    assert(s_last_port == I2C_NUM_0);
    assert(s_last_config.mode == I2C_MODE_MASTER);
    assert(s_last_config.sda_io_num == BOARD_SENSOR_I2C_SDA_GPIO);
    assert(s_last_config.scl_io_num == BOARD_SENSOR_I2C_SCL_GPIO);
    assert(s_last_config.sda_pullup_en == GPIO_PULLUP_ENABLE);
    assert(s_last_config.scl_pullup_en == GPIO_PULLUP_ENABLE);
    assert(s_last_config.master.clk_speed == SENSOR_BUS_I2C_SPEED_HZ);
    assert(s_last_address == BOARD_INA3221_I2C_ADDRESS);
    assert(s_last_device_speed == SENSOR_BUS_I2C_SPEED_HZ);

    const unsigned create_calls = s_bus_create_calls;
    assert(sensor_bus_init() == ESP_OK);
    assert(s_bus_create_calls == create_calls);

    s_device_delete_result = ESP_FAIL;
    s_bus_delete_result = ESP_ERR_TIMEOUT;
    assert(sensor_bus_deinit() == ESP_FAIL);
    assert(s_device_delete_calls == 1U);
    assert(s_bus_delete_calls == 2U);
    assert(sensor_bus_deinit() == ESP_OK);

    s_device_delete_result = ESP_OK;
    s_bus_delete_result = ESP_ERR_TIMEOUT;
    assert(sensor_bus_init() == ESP_OK);
    assert(sensor_bus_deinit() == ESP_ERR_TIMEOUT);

    return 0;
}
