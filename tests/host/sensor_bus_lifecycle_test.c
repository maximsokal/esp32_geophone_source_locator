#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "sensor_bus.h"
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sensor_bus_lines_fake.h"

/*
 * Sequential fixture: the production lock only needs to exist and behave
 * correctly for a single caller here. Real cross-task mutual exclusion is
 * covered by tests/host/sensor_bus_concurrency_test.c.
 */
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

static int s_bitbang_calls_at_bus_create = -1;

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    ++s_bus_create_calls;
    s_bitbang_calls_at_bus_create = g_fake_lines.bitbang_prepare_calls;
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

esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data)
{
    (void)device;
    (void)mem_address;
    if (data != NULL) {
        *data = 0U;
    }
    return ESP_FAIL;
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

/* MAX-243 */
static void test_pre_init_bus_clear_runs_before_i2c0_exists(void)
{
    fake_lines_reset();
    g_fake_lines.sda_hold_clocks = 3; /* target reset mid-byte */
    assert(sensor_bus_init() == ESP_OK);
    /* The bit-bang clear ran on the free pins BEFORE the driver took them. */
    assert(s_bitbang_calls_at_bus_create == 1);
    /* 3 clearing clocks + 1 low phase to set up the STOP. */
    assert(g_fake_lines.scl_driven_low == 4);

    sensor_bus_recovery_status_t status;
    assert(sensor_bus_get_recovery_status(NULL) == ESP_ERR_INVALID_ARG);
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.last_result == I2C_LINES_RECOVERED);
    assert(status.recoveries >= 1U);
    assert(status.consecutive_failures == 0U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_pre_init_stuck_scl_is_recorded_not_clocked(void)
{
    fake_lines_reset();
    g_fake_lines.scl_held = true;
    g_fake_lines.sda_hold_clocks = -1;
    /* The bus is still created; its transfers will fail and be reported. */
    assert(sensor_bus_init() == ESP_OK);
    assert(g_fake_lines.scl_driven_low == 0);
    sensor_bus_recovery_status_t status;
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.last_result == I2C_LINES_SCL_STUCK);
    assert(status.consecutive_failures >= 1U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_runtime_sda_stuck_uses_controller_reset(void)
{
    fake_lines_reset();
    assert(sensor_bus_init() == ESP_OK);
    g_fake_lines.now_us += 5000000;

    /* Healthy live bus: nothing is reset. */
    i2c_line_result_t result = I2C_LINES_SCL_STUCK;
    assert(sensor_bus_recover(&result) == ESP_OK);
    assert(result == I2C_LINES_IDLE);
    assert(g_fake_lines.controller_reset_calls == 0);

    /* A target now holds SDA; the driver's controller reset clears it. */
    g_fake_lines.now_us += 5000000;
    g_fake_lines.sda_hold_clocks = -1;
    g_fake_lines.controller_reset_clears_sda = true;
    assert(sensor_bus_recover(&result) == ESP_OK);
    assert(result == I2C_LINES_RECOVERED);
    assert(g_fake_lines.controller_reset_calls == 1);
    /* The live bus is never bit-banged under the driver's feet. */
    assert(g_fake_lines.bitbang_prepare_calls == 1);

    /* SDA that survives the reset is reported. */
    g_fake_lines.now_us += 5000000;
    g_fake_lines.sda_hold_clocks = -1;
    g_fake_lines.controller_reset_clears_sda = false;
    assert(sensor_bus_recover(&result) == ESP_ERR_TIMEOUT);
    assert(result == I2C_LINES_SDA_STUCK);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_runtime_scl_stuck_is_a_fault_with_backoff(void)
{
    fake_lines_reset();
    assert(sensor_bus_init() == ESP_OK);
    g_fake_lines.now_us += 5000000;
    g_fake_lines.scl_held = true;
    g_fake_lines.sda_hold_clocks = -1;

    i2c_line_result_t result = I2C_LINES_IDLE;
    assert(sensor_bus_recover(&result) == ESP_ERR_TIMEOUT);
    assert(result == I2C_LINES_SCL_STUCK);
    /* No clocks, no controller reset against a held SCL. */
    assert(g_fake_lines.controller_reset_calls == 0);
    assert(g_fake_lines.scl_driven_low == 0);

    /* Hammering is refused until the backoff (2 s after one failure). */
    const int reset_calls = g_fake_lines.controller_reset_calls;
    g_fake_lines.now_us += 500000;
    assert(sensor_bus_recover(&result) == ESP_ERR_INVALID_STATE);
    g_fake_lines.now_us += 1000000;
    assert(sensor_bus_recover(NULL) == ESP_ERR_INVALID_STATE);
    sensor_bus_recovery_status_t status;
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    const uint32_t attempts = status.attempts;
    g_fake_lines.now_us += 1000000;
    assert(sensor_bus_recover(NULL) == ESP_ERR_TIMEOUT);
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.attempts == attempts + 1U);
    assert(status.consecutive_failures >= 2U);
    assert(g_fake_lines.controller_reset_calls == reset_calls);

    /* Target power-cycled: the next allowed attempt clears the streak. */
    g_fake_lines.scl_held = false;
    g_fake_lines.sda_hold_clocks = 0;
    g_fake_lines.now_us += 60000000;
    assert(sensor_bus_recover(&result) == ESP_OK);
    assert(result == I2C_LINES_IDLE);
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.consecutive_failures == 0U);
    assert(sensor_bus_deinit() == ESP_OK);
}

int main(void)
{
    sensor_bus_handles_t handles = {0};

    assert(sensor_bus_lock_init() == ESP_OK);
    assert(sensor_bus_lock_init() == ESP_OK);

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

    s_bus_delete_result = ESP_OK;
    test_pre_init_bus_clear_runs_before_i2c0_exists();
    test_pre_init_stuck_scl_is_recorded_not_clocked();
    test_runtime_sda_stuck_uses_controller_reset();
    test_runtime_scl_stuck_is_a_fault_with_backoff();

    return 0;
}
