#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "sensor_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sensor_bus_lines_fake.h"

struct test_semaphore
{
    int token;
};

static struct test_semaphore s_lock_token;
static int s_task_token;

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

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return &s_lock_token;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    assert(semaphore == &s_lock_token);
    assert(ticks_to_wait == pdMS_TO_TICKS(CONFIG_I2C_MS_TO_WAIT));
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_lock_token);
    return pdTRUE;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return &s_task_token;
}

const char *pcTaskGetName(TaskHandle_t task)
{
    (void)task;
    return "tca-test";
}

typedef struct {
    uint8_t address;
    bool active;
} fake_device_t;

static int s_bus_token;
static int s_master_bus_token;
static fake_device_t s_devices[64];
static size_t s_device_count;
static bool s_present[128];
static bool s_tca_present[128];
static bool s_tca_readback_failure[128];
static uint8_t s_tca_control[128];
static esp_err_t s_probe_error[128];
static uint8_t s_delete_order[64];
static size_t s_delete_count;
static unsigned s_bus_create_calls;
static unsigned s_bus_delete_calls;

static bool scan_created_address(uint8_t address);

static void fake_reset(void)
{
    (void)sensor_bus_deinit();
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_present, 0, sizeof(s_present));
    memset(s_tca_present, 0, sizeof(s_tca_present));
    memset(s_tca_readback_failure, 0, sizeof(s_tca_readback_failure));
    memset(s_tca_control, 0, sizeof(s_tca_control));
    memset(s_delete_order, 0, sizeof(s_delete_order));
    for (size_t i = 0; i < 128U; ++i) {
        s_probe_error[i] = ESP_FAIL;
    }
    s_device_count = 0U;
    s_delete_count = 0U;
    s_bus_create_calls = 0U;
    s_bus_delete_calls = 0U;
    fake_lines_reset();
    for (size_t i = 0; i < 128U; ++i) {
        g_fake_lines.probe_result[i] = ESP_ERR_NOT_FOUND;
    }
    g_fake_lines.probe_configured = true;
}

/*
 * MAX-248: ESP-IDF >= 5.3 (i2c_bus v2 over i2c_master) reports a NACK as
 * ESP_ERR_INVALID_STATE, the same code as a hardware timeout. Only an
 * address probe can tell them apart.
 */
static void idf55_nack_everywhere(void)
{
    for (size_t i = 0; i < 128U; ++i) {
        s_probe_error[i] = ESP_ERR_INVALID_STATE;
        g_fake_lines.probe_result[i] = ESP_ERR_NOT_FOUND;
    }
    g_fake_lines.probe_configured = true;
}

static void fake_add_tca(uint8_t address)
{
    s_present[address] = true;
    s_tca_present[address] = true;
    g_fake_lines.probe_result[address] = ESP_OK;
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

i2c_master_bus_handle_t i2c_bus_get_internal_bus_handle(void *bus)
{
    assert(bus == &s_bus_token);
    return &s_master_bus_token;
}

esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device)
{
    assert(bus == &s_master_bus_token);
    assert(config != NULL && config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(device != NULL);
    assert(s_device_count < 64U);
    fake_device_t *fake = &s_devices[s_device_count++];
    fake->address = (uint8_t)config->device_address;
    fake->active = true;
    *device = fake;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    fake_device_t *fake = device;
    assert(fake != NULL && fake->active);
    fake->active = false;
    s_delete_order[s_delete_count++] = fake->address;
    return ESP_OK;
}

static esp_err_t fake_master_read(
    i2c_master_dev_handle_t device, uint8_t *data, size_t data_len)
{
    fake_device_t *fake = device;
    assert(fake != NULL && fake->active && data != NULL && data_len > 0U);
    if (s_present[fake->address]) {
        data[0] = s_tca_present[fake->address]
            ? s_tca_control[fake->address] : 0U;
        return ESP_OK;
    }
    return s_probe_error[fake->address];
}

esp_err_t i2c_master_receive(
    i2c_master_dev_handle_t device, uint8_t *data, size_t data_len, int timeout_ms)
{
    (void)timeout_ms;
    return fake_master_read(device, data, data_len);
}

esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms)
{
    (void)write_buffer;
    (void)write_size;
    (void)timeout_ms;
    return fake_master_read(device, read_buffer, read_size);
}

esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device,
    const uint8_t *data,
    size_t data_len,
    int timeout_ms)
{
    fake_device_t *fake = device;
    (void)timeout_ms;
    assert(fake != NULL && fake->active && data != NULL && data_len > 0U);
    if (s_tca_present[fake->address] &&
        !(s_tca_readback_failure[fake->address] && data[0] == 0x01U)) {
        s_tca_control[fake->address] = data[0];
    }
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
        *data = s_tca_present[fake->address]
            ? s_tca_control[fake->address]
            : 0U;
        return ESP_OK;
    }
    return s_probe_error[fake->address];
}

esp_err_t i2c_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data
)
{
    fake_device_t *fake = device;
    assert(fake != NULL);
    assert(fake->active);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    if (s_tca_present[fake->address] &&
        !(s_tca_readback_failure[fake->address] && data == 0x01U)) {
        s_tca_control[fake->address] = data;
    }
    return ESP_OK;
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
    assert(s_device_count == 1U); /* NACKed TCA candidates are skipped before device creation. */
    assert(s_delete_count == 0U); /* No candidate devices existed to clean up. */

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
    fake_add_tca(0x73U);

    i2c_bus_device_handle_t first = NULL;
    uint8_t first_address = 0U;
    /* 0x70 NACKs, but discovery skips it and reaches the ACKing 0x73. */
    assert(sensor_bus_get_tca9548a(&first, &first_address) == ESP_OK &&
           !scan_created_address(0x70U));
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
    fake_add_tca(0x71U);
    fake_add_tca(0x74U);

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
    g_fake_lines.probe_result[0x72U] = ESP_OK;

    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_TIMEOUT);
    assert(tca == NULL);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_idf55_nack_means_absent_not_fault(void)
{
    fake_reset();
    idf55_nack_everywhere();
    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_NOT_FOUND);
    assert(tca == NULL);
    assert(g_fake_lines.probe_calls == 6);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_idf55_single_tca_is_found_among_nacks(void)
{
    fake_reset();
    idf55_nack_everywhere();
    fake_add_tca(0x75U);
    i2c_bus_device_handle_t tca = NULL;
    uint8_t address = 0U;
    assert(sensor_bus_get_tca9548a(&tca, &address) == ESP_OK);
    assert(tca != NULL);
    assert(address == 0x75U);
    /* Five NACKs are skipped, then the ACK at 0x75 is probed before creation. */
    assert(g_fake_lines.probe_calls == 6);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_idf55_present_but_failing_is_a_fault(void)
{
    fake_reset();
    idf55_nack_everywhere();
    /* 0x72 ACKs its address but the read fails: not "absent". */
    g_fake_lines.probe_result[0x72U] = ESP_OK;
    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_INVALID_STATE);
    assert(tca == NULL);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_idf55_probe_timeout_is_a_fault(void)
{
    fake_reset();
    idf55_nack_everywhere();
    g_fake_lines.probe_result[0x70U] = ESP_ERR_TIMEOUT;
    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_TIMEOUT);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_classify_transfer_error(void)
{
    fake_reset();
    idf55_nack_everywhere();
    sensor_bus_handles_t handles;
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    g_fake_lines.probe_result[0x31U] = ESP_OK;
    g_fake_lines.probe_result[0x32U] = ESP_ERR_TIMEOUT;

    assert(sensor_bus_classify_transfer_error(0x30U, ESP_OK) == ESP_OK);
    assert(sensor_bus_classify_transfer_error(0x30U, ESP_FAIL) == ESP_ERR_NOT_FOUND);
    assert(sensor_bus_classify_transfer_error(0x30U, ESP_ERR_INVALID_STATE) == ESP_ERR_NOT_FOUND);
    assert(sensor_bus_classify_transfer_error(0x31U, ESP_ERR_INVALID_STATE) == ESP_ERR_INVALID_STATE);
    assert(sensor_bus_classify_transfer_error(0x32U, ESP_ERR_INVALID_STATE) == ESP_ERR_TIMEOUT);
    /* Codes that are not ambiguous are never probed. */
    const int probes = g_fake_lines.probe_calls;
    assert(sensor_bus_classify_transfer_error(0x31U, ESP_ERR_TIMEOUT) == ESP_ERR_TIMEOUT);
    assert(g_fake_lines.probe_calls == probes);
    assert(sensor_bus_deinit() == ESP_OK);
    /* Without a bus nothing can be probed: the error is returned as is. */
    assert(sensor_bus_classify_transfer_error(0x30U, ESP_ERR_INVALID_STATE) == ESP_ERR_INVALID_STATE);
}

static bool scan_created_address(uint8_t address)
{
    for (size_t i = 0; i < s_device_count; ++i) {
        if (s_devices[i].address == address) {
            return true;
        }
    }
    return false;
}

static void test_tca_and_bmp280_at_77_are_distinguished(void)
{
    fake_reset();
    fake_add_tca(0x70U);
    s_present[0x77U] = true; /* BMP280 ACKs, but is never a mux candidate. */

    i2c_bus_device_handle_t tca = NULL;
    uint8_t address = NULL_I2C_DEV_ADDR;
    assert(sensor_bus_get_tca9548a(&tca, &address) == ESP_OK);
    assert(tca != NULL && address == 0x70U);
    assert(!scan_created_address(0x77U));
    assert(s_tca_control[0x70U] == 0U); /* original control state restored */
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_bmp280_addresses_without_tca_are_absent(void)
{
    for (uint8_t address = 0x76U; address <= 0x77U; ++address) {
        fake_reset();
        s_present[address] = true;
        i2c_bus_device_handle_t tca = NULL;
        assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_NOT_FOUND);
        assert(tca == NULL);
        assert(!scan_created_address(address));
        assert(sensor_bus_deinit() == ESP_OK);
    }
}

static void test_tca_with_bad_readback_is_logged_and_rejected(void)
{
    fake_reset();
    fake_add_tca(0x72U);
    s_tca_readback_failure[0x72U] = true;

    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_ERR_NOT_FOUND);
    assert(tca == NULL);
    assert(s_tca_control[0x72U] == 0U);
    assert(g_fake_lines.log_calls == 2U); /* timeout config + rejected candidate */
    assert(strstr(g_fake_lines.last_log, "control readback mismatch") != NULL);
    assert(sensor_bus_deinit() == ESP_OK);
}

int main(void)
{
    assert(sensor_bus_lock_init() == ESP_OK);

    test_absent_tca_is_nonfatal();
    test_single_tca_is_cached_and_owned();
    test_multiple_tca_candidates_are_rejected_and_cleaned();
    test_non_nack_transport_error_is_not_reported_as_absence();
    test_idf55_nack_means_absent_not_fault();
    test_idf55_single_tca_is_found_among_nacks();
    test_idf55_present_but_failing_is_a_fault();
    test_idf55_probe_timeout_is_a_fault();
    test_classify_transfer_error();
    test_tca_and_bmp280_at_77_are_distinguished();
    test_bmp280_addresses_without_tca_are_absent();
    test_tca_with_bad_readback_is_logged_and_rejected();
    return 0;
}
