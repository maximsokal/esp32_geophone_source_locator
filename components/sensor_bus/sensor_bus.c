#include "sensor_bus.h"

#include "board_config.h"


#define SENSOR_BUS_I2C_PORT          I2C_NUM_0


static i2c_bus_handle_t s_bus =
    NULL;

static i2c_bus_device_handle_t s_ina3221 =
    NULL;


esp_err_t sensor_bus_init(void)
{
    if (s_bus != NULL && s_ina3221 != NULL) {
        return ESP_OK;
    }

    /* Recover a partially-created local state before retrying initialization. */
    if (s_ina3221 != NULL) {
        (void)i2c_bus_device_delete(
            &s_ina3221
        );
    }

    if (s_bus != NULL) {
        (void)i2c_bus_delete(
            &s_bus
        );
    }

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_SENSOR_I2C_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BOARD_SENSOR_I2C_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_BUS_I2C_SPEED_HZ,
    };

    s_bus =
        i2c_bus_create(
            SENSOR_BUS_I2C_PORT,
            &config
        );

    if (s_bus == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ina3221 =
        i2c_bus_device_create(
            s_bus,
            BOARD_INA3221_I2C_ADDRESS,
            SENSOR_BUS_I2C_SPEED_HZ
        );

    if (s_ina3221 == NULL) {
        (void)i2c_bus_delete(
            &s_bus
        );

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


esp_err_t sensor_bus_get_handles(
    sensor_bus_handles_t *handles
)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err =
        sensor_bus_init();

    if (err != ESP_OK) {
        return err;
    }

    handles->bus =
        s_bus;
    handles->ina3221 =
        s_ina3221;

    return ESP_OK;
}


esp_err_t sensor_bus_deinit(void)
{
    esp_err_t err =
        ESP_OK;

    if (s_ina3221 != NULL) {
        err =
            i2c_bus_device_delete(
                &s_ina3221
            );
    }

    if (s_bus != NULL) {
        const esp_err_t bus_err =
            i2c_bus_delete(
                &s_bus
            );

        if (err == ESP_OK) {
            err =
                bus_err;
        }
    }

    return err;
}
