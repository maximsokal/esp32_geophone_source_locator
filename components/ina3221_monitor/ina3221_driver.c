#include "ina3221_driver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"


#define INA3221_I2C_PORT              I2C_NUM_0
#define INA3221_I2C_SPEED_HZ          100000u
#define INA3221_I2C_TIMEOUT_MS        100

#define INA3221_REG_CONFIG            0x00u
#define INA3221_REG_SHUNT_CH1         0x01u
#define INA3221_REG_BUS_CH1           0x02u

#define INA3221_CONFIG_CH_ALL          0x7000u
#define INA3221_CONFIG_AVG_16          0x0400u
#define INA3221_CONFIG_VBUS_CT_1_1_MS  0x0100u
#define INA3221_CONFIG_VSH_CT_1_1_MS   0x0020u
#define INA3221_CONFIG_CONT_SHUNT_BUS  0x0007u

#define INA3221_CONFIG_VALUE (
    INA3221_CONFIG_CH_ALL |
    INA3221_CONFIG_AVG_16 |
    INA3221_CONFIG_VBUS_CT_1_1_MS |
    INA3221_CONFIG_VSH_CT_1_1_MS |
    INA3221_CONFIG_CONT_SHUNT_BUS
)

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
        i2c_master_transmit_receive(
            driver->device,
            &register_address,
            sizeof(register_address),
            data,
            sizeof(data),
            INA3221_I2C_TIMEOUT_MS
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
        register_address,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFFu),
    };

    return
        i2c_master_transmit(
            driver->device,
            data,
            sizeof(data),
            INA3221_I2C_TIMEOUT_MS
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

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = INA3221_I2C_PORT,
        .sda_io_num = BOARD_SENSOR_I2C_SDA_GPIO,
        .scl_io_num = BOARD_SENSOR_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err =
        i2c_new_master_bus(
            &bus_config,
            &driver->bus
        );

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_INA3221_I2C_ADDRESS,
        .scl_speed_hz = INA3221_I2C_SPEED_HZ,
    };

    err =
        i2c_master_bus_add_device(
            driver->bus,
            &device_config,
            &driver->device
        );

    if (err != ESP_OK) {
        ina3221_driver_close(driver);
        return err;
    }

    err =
        i2c_master_probe(
            driver->bus,
            BOARD_INA3221_I2C_ADDRESS,
            INA3221_I2C_TIMEOUT_MS
        );

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
        (void)i2c_master_bus_rm_device(
            driver->device
        );

        driver->device =
            NULL;
    }

    if (driver->bus != NULL) {
        (void)i2c_del_master_bus(
            driver->bus
        );

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
            ina3221_policy_current_from_shunt(
                channels[i].shunt_voltage_v,
                INA3221_SHUNT_RESISTANCE_OHM
            );

        channels[i].valid =
            true;
    }

    return ESP_OK;
}
