#include "sensor_bus.h"
#include "sensor_bus_lines.h"
#include "board_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SENSOR_BUS_I2C_PORT I2C_NUM_0
#define SENSOR_BUS_TCA9548A_ADDR_MIN 0x70U
#define SENSOR_BUS_TCA9548A_ADDR_MAX 0x77U
#define SENSOR_BUS_BMP280_ADDR_MIN 0x76U
#define SENSOR_BUS_BMP280_ADDR_MAX 0x77U

static const char *TAG = "sensor_bus";

static i2c_bus_handle_t s_bus = NULL;
static i2c_bus_device_handle_t s_ina3221 = NULL;
static i2c_bus_device_handle_t s_tca9548a = NULL;
static uint8_t s_tca9548a_address = NULL_I2C_DEV_ADDR;

/*
 * s_bus/s_ina3221/s_tca9548a/s_tca9548a_address are shared mutable state
 * read and written from at least two independent FreeRTOS tasks (the
 * INA3221 monitor worker and the nRF topology service worker), both of
 * which call sensor_bus_init() on their own schedule. Every public function
 * below holds this mutex across its full body, including the blocking
 * vendor bus/device creation calls, so it must be a task mutex
 * (portMAX_DELAY wait), not a portMUX spinlock: ESP-IDF requires blocking
 * work to stay outside a critical section.
 */
static SemaphoreHandle_t s_lock = NULL;

/* MAX-243: guarded by s_lock. */
static sensor_bus_recovery_status_t s_recovery;

/* A live bus is judged stuck only if a line reads low on every sample over
 * ~1 ms: at 100 kHz a transfer of another task toggles SCL every 5 us, so a
 * normal transaction or clock stretch cannot look stuck. */
#define SENSOR_BUS_STUCK_SAMPLES 10U
#define SENSOR_BUS_STUCK_SAMPLE_US 100U

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

static esp_err_t sensor_bus_lock(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

static void sensor_bus_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

esp_err_t sensor_bus_lock_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Caller holds s_lock. */
static void sensor_bus_record_recovery(i2c_line_result_t result, int64_t now_us)
{
    ++s_recovery.attempts;
    s_recovery.last_result = result;
    s_recovery.last_attempt_us = now_us;
    if (result == I2C_LINES_RECOVERED) {
        ++s_recovery.recoveries;
    }
    if (result == I2C_LINES_SDA_STUCK || result == I2C_LINES_SCL_STUCK) {
        ++s_recovery.failures;
        ++s_recovery.consecutive_failures;
    } else {
        s_recovery.consecutive_failures = 0U;
    }
}

/* Caller holds s_lock and no I2C driver owns the pins. */
static i2c_line_result_t sensor_bus_bitbang_clear_locked(void)
{
    i2c_line_io_t io;
    if (sensor_bus_lines_bitbang_io(&io) != ESP_OK) {
        return I2C_LINES_SCL_STUCK;
    }
    return i2c_line_recover(&io).result;
}

/* Caller holds s_lock and I2C0 exists. */
static void sensor_bus_sample_live_lines(bool *sda_stuck, bool *scl_stuck)
{
    *sda_stuck = true;
    *scl_stuck = true;
    for (uint32_t i = 0U; i < SENSOR_BUS_STUCK_SAMPLES; ++i) {
        if (i > 0U) {
            sensor_bus_lines_delay_us(SENSOR_BUS_STUCK_SAMPLE_US);
        }
        bool sda_high = false;
        bool scl_high = false;
        sensor_bus_lines_read(&sda_high, &scl_high);
        *sda_stuck = *sda_stuck && !sda_high;
        *scl_stuck = *scl_stuck && !scl_high;
    }
}

/* Caller holds s_lock and I2C0 exists. */
static i2c_line_result_t sensor_bus_live_clear_locked(void)
{
    bool sda_stuck = false;
    bool scl_stuck = false;
    sensor_bus_sample_live_lines(&sda_stuck, &scl_stuck);
    if (scl_stuck) {
        return I2C_LINES_SCL_STUCK;  /* never clock against a held SCL */
    }
    if (!sda_stuck) {
        return I2C_LINES_IDLE;
    }
    if (sensor_bus_lines_controller_reset(s_bus) != ESP_OK) {
        return I2C_LINES_SDA_STUCK;
    }
    sensor_bus_sample_live_lines(&sda_stuck, &scl_stuck);
    if (scl_stuck) {
        return I2C_LINES_SCL_STUCK;
    }
    return sda_stuck ? I2C_LINES_SDA_STUCK : I2C_LINES_RECOVERED;
}

/* Caller holds s_lock and I2C0 exists. */
static esp_err_t sensor_bus_classify_locked(uint8_t address, esp_err_t transfer_err)
{
    if (transfer_err == ESP_FAIL) {
        return ESP_ERR_NOT_FOUND;  /* legacy driver NACK */
    }
    if (transfer_err != ESP_ERR_INVALID_STATE) {
        return transfer_err;
    }
    const esp_err_t probe_err = sensor_bus_lines_probe(s_bus, address);
    if (probe_err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    /* ACK: the target is there and the transfer itself failed. */
    return probe_err == ESP_OK ? transfer_err : probe_err;
}

/* Caller holds s_lock. Confirm the writable TCA control register and leave
 * the mux in exactly the state it had before discovery. */
static bool sensor_bus_verify_tca_candidate_locked(
    i2c_bus_device_handle_t device,
    uint8_t address
)
{
    uint8_t original_mask = 0U;
    esp_err_t err = i2c_bus_read_byte(device, NULL_I2C_MEM_ADDR, &original_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9548A candidate 0x%02X rejected: initial control read failed (%d)",
                 address, (int)err);
        return false;
    }

    const uint8_t test_mask = (uint8_t)(original_mask ^ 0x01U);
    err = i2c_bus_write_byte(device, NULL_I2C_MEM_ADDR, test_mask);
    bool valid = err == ESP_OK;
    const char *reason = "control write failed";
    if (valid) {
        uint8_t readback = 0U;
        err = i2c_bus_read_byte(device, NULL_I2C_MEM_ADDR, &readback);
        if (err != ESP_OK) {
            valid = false;
            reason = "control readback failed";
        } else if (readback != test_mask) {
            valid = false;
            reason = "control readback mismatch";
        }
    }

    const esp_err_t restore_err = i2c_bus_write_byte(
        device, NULL_I2C_MEM_ADDR, original_mask);
    uint8_t restored_mask = 0U;
    const esp_err_t restore_read_err = restore_err == ESP_OK
        ? i2c_bus_read_byte(device, NULL_I2C_MEM_ADDR, &restored_mask)
        : restore_err;
    if (restore_read_err != ESP_OK || restored_mask != original_mask) {
        valid = false;
        reason = "control restore failed";
    }

    if (!valid) {
        ESP_LOGW(TAG, "TCA9548A candidate 0x%02X rejected: %s (%d)",
                 address, reason, (int)err);
    }
    return valid;
}

/* Caller must already hold s_lock. */
static esp_err_t sensor_bus_init_locked(void)
{
    if (s_bus != NULL && s_ina3221 != NULL) {
        return ESP_OK;
    }

    (void)sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        (void)i2c_bus_delete(&s_bus);
    }

    /* MAX-243: a target reset mid-transfer can hold SDA low across our own
     * reboot; clear it while the pins are still free. A stuck line is only
     * recorded -- the bus is still created, and its transfers fail and are
     * reported by their consumers. */
    sensor_bus_record_recovery(
        sensor_bus_bitbang_clear_locked(), sensor_bus_lines_now_us()
    );

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

esp_err_t sensor_bus_init(void)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    const esp_err_t err = sensor_bus_init_locked();
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const esp_err_t err = sensor_bus_init_locked();
    if (err == ESP_OK) {
        handles->bus = s_bus;
        handles->ina3221 = s_ina3221;
    }

    sensor_bus_unlock();
    return err;
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

    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = sensor_bus_init_locked();
    if (err != ESP_OK) {
        sensor_bus_unlock();
        return err;
    }

    if (s_tca9548a != NULL) {
        *device = s_tca9548a;
        if (address != NULL) {
            *address = s_tca9548a_address;
        }
        sensor_bus_unlock();
        return ESP_OK;
    }

    i2c_bus_device_handle_t found = NULL;
    uint8_t found_address = NULL_I2C_DEV_ADDR;

    for (uint8_t candidate_address = SENSOR_BUS_TCA9548A_ADDR_MIN;
         candidate_address <= SENSOR_BUS_TCA9548A_ADDR_MAX;
         ++candidate_address) {
        /* BMP280 shares the main bus and may occupy either address. */
        if (candidate_address >= SENSOR_BUS_BMP280_ADDR_MIN &&
            candidate_address <= SENSOR_BUS_BMP280_ADDR_MAX) {
            continue;
        }

        i2c_bus_device_handle_t candidate = i2c_bus_device_create(
            s_bus, candidate_address, SENSOR_BUS_I2C_SPEED_HZ);
        if (candidate == NULL) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }

        /* MAX-248: a NACK is ESP_ERR_NOT_FOUND after classification,
         * whatever code the backend used for it. */
        uint8_t response = 0U;
        const esp_err_t probe_err = sensor_bus_classify_locked(
            candidate_address,
            i2c_bus_read_byte(candidate, NULL_I2C_MEM_ADDR, &response));

        if (probe_err == ESP_OK &&
            sensor_bus_verify_tca_candidate_locked(candidate, candidate_address)) {
            if (found != NULL) {
                const esp_err_t candidate_delete_err = i2c_bus_device_delete(&candidate);
                const esp_err_t found_delete_err = i2c_bus_device_delete(&found);
                sensor_bus_unlock();
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
        if (probe_err != ESP_ERR_NOT_FOUND && probe_err != ESP_OK) {
            err = probe_err;
            goto fail;
        }
    }

    if (found == NULL) {
        sensor_bus_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    s_tca9548a = found;
    s_tca9548a_address = found_address;
    *device = s_tca9548a;
    if (address != NULL) {
        *address = s_tca9548a_address;
    }
    sensor_bus_unlock();
    return ESP_OK;

fail:
    if (found != NULL) {
        const esp_err_t cleanup_err = i2c_bus_device_delete(&found);
        sensor_bus_unlock();
        if (cleanup_err != ESP_OK) {
            return cleanup_err;
        }
        return err;
    }
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_classify_transfer_error(uint8_t address, esp_err_t transfer_err)
{
    if (transfer_err == ESP_OK) {
        return ESP_OK;
    }
    if (transfer_err != ESP_ERR_INVALID_STATE) {
        return transfer_err == ESP_FAIL ? ESP_ERR_NOT_FOUND : transfer_err;
    }
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return transfer_err;
    }
    const esp_err_t err = s_bus != NULL
        ? sensor_bus_classify_locked(address, transfer_err)
        : transfer_err;
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_recover(i2c_line_result_t *result)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const int64_t now_us = sensor_bus_lines_now_us();
    if (!i2c_line_recovery_backoff_allows(
            now_us, s_recovery.last_attempt_us, s_recovery.consecutive_failures)) {
        sensor_bus_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_line_result_t line_result = s_bus != NULL
        ? sensor_bus_live_clear_locked()
        : sensor_bus_bitbang_clear_locked();
    sensor_bus_record_recovery(line_result, now_us);
    sensor_bus_unlock();

    if (result != NULL) {
        *result = line_result;
    }
    return line_result == I2C_LINES_IDLE || line_result == I2C_LINES_RECOVERED
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

esp_err_t sensor_bus_get_recovery_status(sensor_bus_recovery_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    memcpy(status, &s_recovery, sizeof(*status));
    sensor_bus_unlock();
    return ESP_OK;
}

esp_err_t sensor_bus_deinit(void)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        const esp_err_t bus_err = i2c_bus_delete(&s_bus);
        if (err == ESP_OK) {
            err = bus_err;
        }
    }

    sensor_bus_unlock();
    return err;
}
