#include "ina3221_driver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INA3221_REG_CONFIG            0x00u
#define INA3221_REG_SHUNT_CH1         0x01u
#define INA3221_REG_BUS_CH1           0x02u

#define INA3221_CONFIG_CH_ALL          0x7000u
#define INA3221_CONFIG_AVG_16          0x0400u
#define INA3221_CONFIG_VBUS_CT_1_1_MS  0x0100u
#define INA3221_CONFIG_VSH_CT_1_1_MS   0x0020u
#define INA3221_CONFIG_CONT_SHUNT_BUS  0x0007u

#define INA3221_CONFIG_VALUE \
    (INA3221_CONFIG_CH_ALL | \
     INA3221_CONFIG_AVG_16 | \
     INA3221_CONFIG_VBUS_CT_1_1_MS | \
     INA3221_CONFIG_VSH_CT_1_1_MS | \
     INA3221_CONFIG_CONT_SHUNT_BUS)

#define INA3221_SHUNT_RESISTANCE_OHM  0.1f


static esp_err_t ina3221_driver_read_register(
    ina3221_driver_t *driver,
    uint8_t register_address,
    uint16_t *value
)
{
    if (
        driver == NULL ||
        driver->device == NULL ||
        value == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2] = { 0 };

    esp_err_t err =
        i2c_bus_read_bytes(
            driver->device,
            register_address,
            sizeof(data),
            data
        );

    if (err != ESP_OK) {
        return err;
    }

    *value =
        ((uint16_t)data[0] << 8) |
        (uint16_t)data[1];

    return ESP_OK;
}


static esp_err_t ina3221_driver_write_register(
    ina3221_driver_t *driver,
    uint8_t register_address,
    uint16_t value
)
{
    if (
        driver == NULL ||
        driver->device == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t data[] = {
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFFu),
    };

    return
        i2c_bus_write_bytes(
            driver->device,
            register_address,
            sizeof(data),
            data
        );
}


esp_err_t ina3221_driver_open(
    ina3221_driver_t *driver
)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ina3221_driver_close(driver);

    sensor_bus_handles_t handles = { 0 };

    esp_err_t err =
        sensor_bus_get_handles(
            &handles
        );

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    driver->bus =
        handles.bus;
    driver->device =
        handles.ina3221;

    uint16_t probe_value = 0;

    err =
        ina3221_driver_read_register(
            driver,
            INA3221_REG_CONFIG,
            &probe_value
        );

    /* A successful register read is the shared-bus equivalent of probe. */
    (void)probe_value;

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    err =
        ina3221_driver_write_register(
            driver,
            INA3221_REG_CONFIG,
            INA3221_CONFIG_VALUE
        );

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    uint16_t config_readback = 0;

    err =
        ina3221_driver_read_register(
            driver,
            INA3221_REG_CONFIG,
            &config_readback
        );

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    if (config_readback != INA3221_CONFIG_VALUE) {
        ina3221_driver_close(driver);
        return ESP_ERR_INVALID_RESPONSE;
    }

    driver->ready =
        true;

    return ESP_OK;
}


void ina3221_driver_close(
    ina3221_driver_t *driver
)
{
    if (driver == NULL) {
        return;
    }

    driver->ready =
        false;

    if (driver->device != NULL) {
        driver->device =
            NULL;
    }

    if (driver->bus != NULL) {
        driver->bus =
            NULL;
    }
}


esp_err_t ina3221_driver_read_channels(
    ina3221_driver_t *driver,
    ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT]
)
{
    if (
        driver == NULL ||
        !driver->ready ||
        channels == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(
        channels,
        0,
        sizeof(*channels) *
            INA3221_POLICY_CHANNEL_COUNT
    );

    for (
        size_t i = 0;
        i < INA3221_POLICY_CHANNEL_COUNT;
        ++i
    ) {
        const uint8_t shunt_register =
            (uint8_t)(
                INA3221_REG_SHUNT_CH1 +
                (2u * i)
            );

        const uint8_t bus_register =
            (uint8_t)(
                INA3221_REG_BUS_CH1 +
                (2u * i)
            );

        uint16_t shunt_raw = 0;
        uint16_t bus_raw = 0;

        esp_err_t err =
            ina3221_driver_read_register(
                driver,
                shunt_register,
                &shunt_raw
            );

        if (err != ESP_OK) {
            return err;
        }

        err =
            ina3221_driver_read_register(
                driver,
                bus_register,
                &bus_raw
            );

        if (err != ESP_OK) {
            return err;
        }

        channels[i].shunt_voltage_v =
            ina3221_policy_shunt_raw_to_volts(
                shunt_raw
            );

        channels[i].bus_voltage_v =
            ina3221_policy_bus_raw_to_volts(
                bus_raw
            );

        channels[i].current_a =
            ina3221_policy_current_for_channel(
                i,
                channels[i].shunt_voltage_v,
                INA3221_SHUNT_RESISTANCE_OHM
            );

        channels[i].valid =
            true;
    }

    return ESP_OK;
}
