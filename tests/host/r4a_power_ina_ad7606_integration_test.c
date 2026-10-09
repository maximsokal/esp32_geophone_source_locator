#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ad7606_power_policy.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "ina3221_driver.h"
#include "power_control.h"
#include "sensor_bus.h"

static int s_bus_token;
static int s_device_token;
static uint16_t s_registers[0x10];
static gpio_mode_t s_gpio_mode;
static uint32_t s_gpio_register;
static bool s_pad_high;
static uint32_t s_now_ms;
static uint32_t s_high_since_ms;
static unsigned s_high_edges;
static unsigned s_low_edges;
static uint32_t s_pulse_duration_ms;
static bool s_shutdown_requested;
static unsigned s_button_stop_calls;

esp_err_t power_button_adapter_init(void (*hook)(void *), void *user_context);
esp_err_t power_button_adapter_arm_after_startup(void);
esp_err_t power_button_adapter_set_notification_hook(void (*hook)(void *), void *user_context);
void power_button_adapter_wait_for_shutdown_request(void);
bool power_button_adapter_is_shutdown_requested(void);
void power_button_adapter_stop(void);
esp_err_t power_button_adapter_request_shutdown(void);

static bool near(float left, float right)
{
    return fabsf(left - right) < 1e-5f;
}

static void reset_world(void)
{
    memset(s_registers, 0, sizeof(s_registers));
    s_registers[0x00] = 0x7127U;
    s_gpio_mode = GPIO_MODE_DISABLE;
    s_gpio_register = 0U;
    s_pad_high = false;
    s_now_ms = 1000U;
    s_high_since_ms = 0U;
    s_high_edges = 0U;
    s_low_edges = 0U;
    s_pulse_duration_ms = 0U;
    s_shutdown_requested = false;
    s_button_stop_calls = 0U;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    assert(handles != NULL);
    handles->bus = &s_bus_token;
    handles->ina3221 = &s_device_token;
    return ESP_OK;
}

esp_err_t sensor_bus_probe_address_with_context(uint8_t address, const char *source, int channel)
{
    assert(address == 0x40U);
    assert(source != NULL && strcmp(source, "ina3221") == 0);
    assert(channel == -1);
    return ESP_OK;
}

esp_err_t sensor_bus_classify_transfer_error_with_context(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel)
{
    assert(address == 0x40U);
    assert(source != NULL && strcmp(source, "ina3221") == 0);
    assert(channel == -1);
    return transfer_err;
}

esp_err_t i2c_bus_read_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data)
{
    assert(device == &s_device_token);
    assert(mem_address < sizeof(s_registers));
    assert(data_len == 2U);
    data[0] = (uint8_t)(s_registers[mem_address] >> 8U);
    data[1] = (uint8_t)(s_registers[mem_address] & 0xFFU);
    return ESP_OK;
}

esp_err_t i2c_bus_write_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    const uint8_t *data)
{
    assert(device == &s_device_token);
    assert(mem_address < sizeof(s_registers));
    assert(data_len == 2U);
    s_registers[mem_address] = (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config != NULL);
    assert(config->pin_bit_mask == (1ULL << BOARD_POWER_OFF_GPIO));
    s_gpio_mode = config->mode;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    assert(gpio_num == BOARD_POWER_OFF_GPIO);
    s_gpio_register = level;
    const bool high = s_gpio_mode == GPIO_MODE_OUTPUT && level != 0U;
    if (high && !s_pad_high) {
        ++s_high_edges;
        s_high_since_ms = s_now_ms;
    } else if (!high && s_pad_high) {
        ++s_low_edges;
        s_pulse_duration_ms = s_now_ms - s_high_since_ms;
    }
    s_pad_high = high;
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    s_now_ms += ticks_to_delay;
}

esp_err_t power_button_adapter_init(void (*hook)(void *), void *user_context)
{
    (void)hook;
    (void)user_context;
    return ESP_OK;
}

esp_err_t power_button_adapter_arm_after_startup(void)
{
    return ESP_OK;
}

esp_err_t power_button_adapter_set_notification_hook(void (*hook)(void *), void *user_context)
{
    (void)hook;
    (void)user_context;
    return ESP_OK;
}

void power_button_adapter_wait_for_shutdown_request(void)
{
}

bool power_button_adapter_is_shutdown_requested(void)
{
    return s_shutdown_requested;
}

void power_button_adapter_stop(void)
{
    ++s_button_stop_calls;
}

esp_err_t power_button_adapter_request_shutdown(void)
{
    s_shutdown_requested = true;
    return ESP_OK;
}

static void test_low_ina_rail_gates_ad7606_and_orders_power_shutdown(void)
{
    reset_world();
    const uint16_t ad7606_bus_counts = 575U; /* 575 * 8 mV = 4.600 V */
    s_registers[0x06] = (uint16_t)(ad7606_bus_counts << 3U);

    ina3221_driver_t driver = { 0 };
    assert(ina3221_driver_open(&driver) == ESP_OK);
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT];
    assert(ina3221_driver_read_channels(&driver, channels) == ESP_OK);
    assert(channels[INA3221_POLICY_AD7606_INDEX].valid);
    assert(near(channels[INA3221_POLICY_AD7606_INDEX].bus_voltage_v, 4.600f));

    const ad7606_power_gate_t gate = ad7606_power_gate(
        true,
        channels[INA3221_POLICY_AD7606_INDEX].valid,
        channels[INA3221_POLICY_AD7606_INDEX].bus_voltage_v,
        1000000LL,
        1000000LL);
    assert(gate == AD7606_POWER_GATE_RAIL_LOW);

    assert(power_control_init() == ESP_OK);
    assert(!s_pad_high && s_high_edges == 0U);
    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY) == ESP_OK);
    assert(power_control_shutdown_reason() == POWER_SHUTDOWN_REASON_LOW_BATTERY);
    assert(!s_pad_high && s_high_edges == 0U); /* request only latches the shutdown */

    power_control_shutdown();
    assert(s_button_stop_calls == 1U);
    assert(s_high_edges == 1U && s_low_edges == 1U);
    assert(s_pulse_duration_ms == 200U);
    assert(!s_pad_high && s_gpio_register == 0U);
    ina3221_driver_close(&driver);
}

int main(void)
{
    test_low_ina_rail_gates_ad7606_and_orders_power_shutdown();
    return 0;
}
