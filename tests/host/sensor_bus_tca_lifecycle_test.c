#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "sensor_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct test_semaphore
{
    int token;
};

static struct test_semaphore s_lock_token;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_lock_token;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    assert(semaphore == &s_lock_token);
    assert(ticks_to_wait == portMAX_DELAY);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_lock_token);
    return pdTRUE;
}

typedef struct {
    uint8_t address;
    bool active;
} fake_device_t;

static int s_bus_token;
static fake_device_t s_devices[64];
static size_t s_device_count;
static bool s_present[128];
static esp_err_t s_probe_error[128];
static uint8_t s_delete_order[64];
static size_t s_delete_count;
static unsigned s_bus_create_calls;
static unsigned s_bus_delete_calls;

static void fake_reset(void)
{
    (void)sensor_bus_deinit();
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_present, 0, sizeof(s_present));
    memset(s_delete_order, 0, sizeof(s_delete_order));
    for (size_t i = 0; i < 128U; ++i) {
        s_probe_error[i] = ESP_FAIL;
    }
    s_device_count = 0U;
    s_delete_count = 0U;
    s_bus_create_calls = 0U;
    s_bus_delete_calls = 0U;
}

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    assert(port == I2C_NUM_0);
    assert(config != NULL);
    assert(config->sda_io_num == BOARD_SENSOR_I2C_SDA_GPIO);
    assert(config->scl_io_num == BOARD_SENSOR_I2C_SCL_GPIO);
    assert(config->master.clk_speed == SENSOR_BUS_I2C_SPEED_HZ);
    ++s_bus_create_calls;
    return &s_bus_token;
}

esp_err_t i2c_bus_delete(i2c_bus_handle_t *bus)
{
    assert(bus != NULL);
    assert(*bus == &s_bus_token);
    ++s_bus_delete_calls;
    *bus = NULL;
    return ESP_OK;
}

i2c_bus_device_handle_t i2c_bus_device_create(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed
)
{
    assert(bus == &s_bus_token);
    assert(speed == SENSOR_BUS_I2C_SPEED_HZ);
    assert(s_device_count < 64U);
    fake_device_t *device = &s_devices[s_device_count++];
    device->address = address;
    device->active = true;
    return device;
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    assert(device != NULL);
    assert(*device != NULL);
    fake_device_t *fake = *device;
    assert(fake->active);
    fake->active = false;
    s_delete_order[s_delete_count++] = fake->address;
    *device = NULL;
    return ESP_OK;
}

esp_err_t i2c_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data
)
{
    fake_device_t *fake = device;
    assert(fake != NULL);
    assert(fake->active);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    assert(data != NULL);

    if (s_present[fake->address]) {
        *data = 0U;
        return ESP_OK;
    }
    return s_probe_error[fake->address];
}

static void test_absent_tca_is_nonfatal(void)
{
    fake_reset();

    int sentinel;
    i2c_bus_device_handle_t tca = &sentinel;
    uint8_t address = 0U;
    assert(sensor_bus_get_tca9548a(&tca, &address) == ESP_ERR_NOT_FOUND);
    assert(tca == NULL);
    assert(address == NULL_I2C_DEV_ADDR);
    assert(s_bus_create_calls == 1U);
    assert(s_device_count == 9U); /* INA3221 + exactly eight TCA candidates. */
    assert(s_delete_count == 8U);

    sensor_bus_handles_t handles;
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    assert(handles.bus == &s_bus_token);
    assert(handles.ina3221 != NULL);

    assert(sensor_bus_deinit() == ESP_OK);
    assert(s_delete_order[s_delete_count - 1U] == BOARD_INA3221_I2C_ADDRESS);
    assert(s_bus_delete_calls == 1U);
}

static void test_single_tca_is_cached_and_owned(void)
{
    fake_reset();
    s_present[0x73U] = true;

    i2c_bus_device_handle_t first = NULL;
    uint8_t first_address = 0U;
    assert(sensor_bus_get_tca9548a(&first, &first_address) == ESP_OK);
    assert(first != NULL);
    assert(first_address == 0x73U);

    const size_t created_after_discovery = s_device_count;
    i2c_bus_device_handle_t second = NULL;
    uint8_t second_address = 0U;
    assert(sensor_bus_get_tca9548a(&second, &second_address) == ESP_OK);
    assert(second == first);
    assert(second_address == first_address);
    assert(s_device_count == created_after_discovery);

    assert(sensor_bus_deinit() == ESP_OK);
    assert(s_delete_order[s_delete_count - 2U] == 0x73U);
    assert(s_delete_order[s_delete_count - 1U] == BOARD_INA3221_I2C_ADDRESS);
}

static void test_multiple_tca_candidates_are_rejected_and_cleaned(void)
{
    fake_reset();
    s_present[0x71U] = true;
    s_present[0x74U] = true;

    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_INVALID_STATE);
    assert(tca == NULL);
    assert(s_delete_order[s_delete_count - 2U] == 0x74U);
    assert(s_delete_order[s_delete_count - 1U] == 0x71U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_non_nack_transport_error_is_not_reported_as_absence(void)
{
    fake_reset();
    s_probe_error[0x72U] = ESP_ERR_TIMEOUT;

    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_TIMEOUT);
    assert(tca == NULL);
    assert(sensor_bus_deinit() == ESP_OK);
}

int main(void)
{
    assert(sensor_bus_lock_init() == ESP_OK);

    test_absent_tca_is_nonfatal();
    test_single_tca_is_cached_and_owned();
    test_multiple_tca_candidates_are_rejected_and_cleaned();
    test_non_nack_transport_error_is_not_reported_as_absence();
    return 0;
}
