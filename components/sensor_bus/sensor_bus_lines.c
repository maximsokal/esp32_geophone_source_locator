#include "sensor_bus_lines.h"
#include "board_config.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool line_read(void *context, gpio_num_t pin)
{
    (void)context;
    return gpio_get_level(pin) != 0;
}

static bool read_sda(void *context)
{
    return line_read(context, BOARD_SENSOR_I2C_SDA_GPIO);
}

static bool read_scl(void *context)
{
    return line_read(context, BOARD_SENSOR_I2C_SCL_GPIO);
}

static void set_sda(void *context, bool released)
{
    (void)context;
    (void)gpio_set_level(BOARD_SENSOR_I2C_SDA_GPIO, released ? 1U : 0U);
}

static void set_scl(void *context, bool released)
{
    (void)context;
    (void)gpio_set_level(BOARD_SENSOR_I2C_SCL_GPIO, released ? 1U : 0U);
}

static void delay_us(void *context, uint32_t us)
{
    (void)context;
    esp_rom_delay_us(us);
}


esp_err_t sensor_bus_lines_bitbang_io(i2c_line_io_t *io)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Released level loaded before the pins become open-drain outputs, so
     * taking them never pulls a line low. */
    esp_err_t err = gpio_set_level(BOARD_SENSOR_I2C_SDA_GPIO, 1U);
    if (err == ESP_OK) {
        err = gpio_set_level(BOARD_SENSOR_I2C_SCL_GPIO, 1U);
    }
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t config = {
        .pin_bit_mask =
            (1ULL << BOARD_SENSOR_I2C_SDA_GPIO) |
            (1ULL << BOARD_SENSOR_I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    *io = (i2c_line_io_t) {
        .read_sda = read_sda,
        .read_scl = read_scl,
        .set_sda = set_sda,
        .set_scl = set_scl,
        .delay_us = delay_us,
        .context = NULL,
    };
    return ESP_OK;
}


void sensor_bus_lines_read(bool *sda_high, bool *scl_high)
{
    /* The I2C driver keeps its pins input-enabled, so the GPIO input
     * register reflects the real line levels. */
    *sda_high = gpio_get_level(BOARD_SENSOR_I2C_SDA_GPIO) != 0;
    *scl_high = gpio_get_level(BOARD_SENSOR_I2C_SCL_GPIO) != 0;
}


esp_err_t sensor_bus_lines_controller_reset(i2c_bus_handle_t bus)
{
    i2c_master_bus_handle_t handle = i2c_bus_get_internal_bus_handle(bus);
    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_bus_reset(handle);
}


esp_err_t sensor_bus_lines_probe(i2c_bus_handle_t bus, uint8_t address)
{
    i2c_master_bus_handle_t handle = i2c_bus_get_internal_bus_handle(bus);
    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(handle, address, SENSOR_BUS_LINES_PROBE_TIMEOUT_MS);
}


uint32_t sensor_bus_lines_boost_task_priority(uint32_t priority)
{
    const uint32_t previous = (uint32_t)uxTaskPriorityGet(NULL);

    if (priority > previous) {
        vTaskPrioritySet(NULL, (UBaseType_t)priority);
    }
    return previous;
}

void sensor_bus_lines_restore_task_priority(uint32_t previous_priority)
{
    if ((uint32_t)uxTaskPriorityGet(NULL) != previous_priority) {
        vTaskPrioritySet(NULL, (UBaseType_t)previous_priority);
    }
}

void sensor_bus_lines_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}


int64_t sensor_bus_lines_now_us(void)
{
    return esp_timer_get_time();
}
