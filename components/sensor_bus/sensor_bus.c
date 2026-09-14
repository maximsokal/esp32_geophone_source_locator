#include "sensor_bus.h"
#include "board_config.h"

#define SENSOR_BUS_I2C_PORT I2C_NUM_0
#define SENSOR_BUS_TCA9548A_ADDR_MIN 0x70U
#define SENSOR_BUS_TCA9548A_ADDR_MAX 0x77U

static i2c_bus_handle_t s_bus = NULL;
static i2c_bus_device_handle_t s_ina3221 = NULL;
static i2c_bus_device_handle_t s_tca9548a = NULL;
static uint8_t s_tca9548a_address = NULL_I2C_DEV_ADDR;

static esp_err_t sensor_bus_delete_device(i2c_bus_device_handle_t *device)
{
    if (device == NULL || *device == NULL) {
        return ESP_OK;
    }
    return i2c_bus_device_delete(device);
}

static void sensor_bus_clear_tca_cache(void)
{
    s_tca9548a = NULL;
    s_tca9548a_address = NULL_I2C_DEV_ADDR;
}

static esp_err_t sensor_bus_cleanup_owned_devices(void)
{
    esp_err_t err = sensor_bus_delete_device(&s_tca9548a);
    sensor_bus_clear_tca_cache();

    const esp_err_t ina_err = sensor_bus_delete_device(&s_ina3221);
    if (err == ESP_OK) {
        err = ina_err;
    }
    return err;
}

esp_err_t sensor_bus_init(void)
{
    if (s_bus != NULL && s_ina3221 != NULL) {
        return ESP_OK;
    }

    (void)sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        (void)i2c_bus_delete(&s_bus);
    }

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_SENSOR_I2C_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BOARD_SENSOR_I2C_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_BUS_I2C_SPEED_HZ,
    };

    s_bus = i2c_bus_create(SENSOR_BUS_I2C_PORT, &config);
    if (s_bus == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ina3221 = i2c_bus_device_create(s_bus, BOARD_INA3221_I2C_ADDRESS, SENSOR_BUS_I2C_SPEED_HZ);
    if (s_ina3221 == NULL) {
        (void)i2c_bus_delete(&s_bus);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = sensor_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    handles->bus = s_bus;
    handles->ina3221 = s_ina3221;
    return ESP_OK;
}

esp_err_t sensor_bus_get_tca9548a(i2c_bus_device_handle_t *device, uint8_t *address)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = NULL;
    if (address != NULL) {
        *address = NULL_I2C_DEV_ADDR;
    }

    esp_err_t err = sensor_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_tca9548a != NULL) {
        *device = s_tca9548a;
        if (address != NULL) {
            *address = s_tca9548a_address;
        }
        return ESP_OK;
    }

    i2c_bus_device_handle_t found = NULL;
    uint8_t found_address = NULL_I2C_DEV_ADDR;

    for (uint8_t candidate_address = SENSOR_BUS_TCA9548A_ADDR_MIN;
         candidate_address <= SENSOR_BUS_TCA9548A_ADDR_MAX;
         ++candidate_address) {
        i2c_bus_device_handle_t candidate = i2c_bus_device_create(
            s_bus, candidate_address, SENSOR_BUS_I2C_SPEED_HZ);
        if (candidate == NULL) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }

        uint8_t control = 0U;
        const esp_err_t probe_err = i2c_bus_read_byte(candidate, NULL_I2C_MEM_ADDR, &control);
        (void)control;

        if (probe_err == ESP_OK) {
            if (found != NULL) {
                const esp_err_t candidate_delete_err = i2c_bus_device_delete(&candidate);
                const esp_err_t found_delete_err = i2c_bus_device_delete(&found);
                if (candidate_delete_err != ESP_OK) {
                    return candidate_delete_err;
                }
                if (found_delete_err != ESP_OK) {
                    return found_delete_err;
                }
                return ESP_ERR_INVALID_STATE;
            }
            found = candidate;
            found_address = candidate_address;
            continue;
        }

        const esp_err_t delete_err = i2c_bus_device_delete(&candidate);
        if (delete_err != ESP_OK) {
            err = delete_err;
            goto fail;
        }
        if (probe_err != ESP_FAIL) {
            err = probe_err;
            goto fail;
        }
    }

    if (found == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    s_tca9548a = found;
    s_tca9548a_address = found_address;
    *device = s_tca9548a;
    if (address != NULL) {
        *address = s_tca9548a_address;
    }
    return ESP_OK;

fail:
    if (found != NULL) {
        const esp_err_t cleanup_err = i2c_bus_device_delete(&found);
        if (cleanup_err != ESP_OK) {
            return cleanup_err;
        }
    }
    return err;
}

esp_err_t sensor_bus_deinit(void)
{
    esp_err_t err = sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        const esp_err_t bus_err = i2c_bus_delete(&s_bus);
        if (err == ESP_OK) {
            err = bus_err;
        }
    }
    return err;
}
