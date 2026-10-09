#include "tca9548a_mux_i2c_bus.h"
#include "sensor_bus_probe_diag.h"

#include <stddef.h>

#ifndef CONFIG_APP_I2C_PROBE_DIAGNOSTICS
#define CONFIG_APP_I2C_PROBE_DIAGNOSTICS 0
#endif

static esp_err_t tca9548a_i2c_write_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)rx;
    (void)rx_len;
    if (context == NULL || tx == NULL || tx_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_write_byte((i2c_bus_device_handle_t)context,
                              NULL_I2C_MEM_ADDR, tx[0]);
}

static esp_err_t tca9548a_i2c_read_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)tx;
    (void)tx_len;
    if (context == NULL || rx == NULL || rx_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_read_byte((i2c_bus_device_handle_t)context,
                             NULL_I2C_MEM_ADDR, rx);
}

#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
static sensor_bus_probe_phase_t tca9548a_diag_phase(tca9548a_mux_phase_t phase)
{
    switch (phase) {
        case TCA9548A_MUX_PHASE_CLOSE:
            return SENSOR_BUS_PROBE_PHASE_TCA_CLOSE;
        case TCA9548A_MUX_PHASE_SELECT:
            return SENSOR_BUS_PROBE_PHASE_TCA_SELECT;
        case TCA9548A_MUX_PHASE_VERIFY:
        default:
            return SENSOR_BUS_PROBE_PHASE_TCA_VERIFY;
    }
}
#endif

static esp_err_t tca9548a_i2c_write(
    void *context,
    uint8_t address,
    uint8_t value,
    tca9548a_mux_phase_t phase,
    int channel)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    return sensor_bus_probe_diag_transaction(
        tca9548a_diag_phase(phase), address, channel, &value, 1U,
        NULL, 0U, true, value, tca9548a_i2c_write_transfer, context);
#else
    (void)address;
    (void)phase;
    (void)channel;
    return tca9548a_i2c_write_transfer(context, &value, 1U, NULL, 0U);
#endif
}

static esp_err_t tca9548a_i2c_read(
    void *context,
    uint8_t address,
    uint8_t *value,
    uint8_t expected,
    tca9548a_mux_phase_t phase,
    int channel)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    return sensor_bus_probe_diag_transaction(
        tca9548a_diag_phase(phase), address, channel, NULL, 0U,
        value, 1U, true, expected, tca9548a_i2c_read_transfer, context);
#else
    (void)address;
    (void)expected;
    (void)phase;
    (void)channel;
    return tca9548a_i2c_read_transfer(context, NULL, 0U, value, 1U);
#endif
}

esp_err_t tca9548a_mux_bind_i2c_bus(
    tca9548a_mux_t *mux,
    i2c_bus_device_handle_t device,
    uint8_t address
)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const tca9548a_mux_io_t io = {
        .write_control = tca9548a_i2c_write,
        .read_control = tca9548a_i2c_read,
        .context = device,
        .address = address,
    };
    return tca9548a_mux_init(mux, &io);
}
