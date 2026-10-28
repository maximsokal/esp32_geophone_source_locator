#include "main_barometer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_tick.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nrf_baro_altitude.h"

#ifndef CONFIG_MAIN_BAROMETER_BMP280_ADDRESS
#define CONFIG_MAIN_BAROMETER_BMP280_ADDRESS 0x77
#endif

#define BMP280_ADDRESS_76 0x76U
#define BMP280_ADDRESS_77 0x77U
#define BMP280_REG_CHIP_ID 0xD0U
#define BMP280_REG_CALIB 0x88U
#define BMP280_REG_CONFIG 0xF5U
#define BMP280_REG_CTRL_MEAS 0xF4U
#define BMP280_REG_STATUS 0xF3U
#define BMP280_REG_DATA 0xF7U
#define BMP280_CHIP_ID 0x58U
#define BME280_CHIP_ID 0x60U
#define BMP280_CONFIG_FILTER_X16_STANDBY_62_5_MS 0x30U
#define BMP280_CTRL_TEMP_X2_PRESS_X16_NORMAL 0x57U
#define BMP280_CTRL_SLEEP 0x00U
#define BMP280_CTRL_TEMP_X2_PRESS_X16_FORCED 0x55U
/* T x2 + P x16: maximum 43.2 ms, plus scheduling margin. */
#define BMP280_FORCED_CONVERSION_MS 50U
#define BMP280_CONVERSION_TIMEOUT_MS 300U
#define MAIN_BAROMETER_FORCED_FALLBACK_POLLS 10U
#define BMP280_PRESSURE_MIN_PA 30000U
#define BMP280_PRESSURE_MAX_PA 110000U
#define BMP280_POLL_PERIOD_MS 200U
#define MAIN_BAROMETER_RETRY_BACKOFF_MS 5000U
/* No proven new sample for this many 200 ms intervals: write config and mode again. */
#define MAIN_BAROMETER_PENDING_REARM_POLLS 5U
/* ... and this many (3 s) without recovery: a height-reference capture is invalid. */
#define MAIN_BAROMETER_PENDING_INVALID_POLLS 15U

#define AHT20_ADDRESS 0x38U
#define AHT20_STATUS_CALIBRATED 0x08U
#define AHT20_STATUS_BUSY 0x80U
#define AHT20_CMD_INITIALIZE 0xBEU
#define AHT20_CMD_MEASURE 0xACU
#define AHT20_INIT_DELAY_MS 10U
#define AHT20_BUSY_POLL_MS 2U
#define AHT20_TIMEOUT_MS 100U
#define AHT20_POLL_PERIOD_MS 1000U
#define MAIN_HEIGHT_REFERENCE_WINDOW_MS 2000U
/* A 2 Pa filtered-pressure range is about 167 mm on the shared 12 Pa/m model. */
#define MAIN_HEIGHT_REFERENCE_MAX_PRESSURE_RANGE_PA 2.0

static const char *TAG = "MAIN_BARO";

static atomic_bool s_polling_requested;
static _Atomic uint16_t s_height_reference_request_generation;

static esp_err_t main_barometer_classify_error(uint8_t address, esp_err_t err)
{
    return err == ESP_OK ? ESP_OK : sensor_bus_classify_transfer_error(address, err);
}

static esp_err_t main_barometer_read_regs(
    i2c_bus_device_handle_t device,
    uint8_t address,
    uint8_t reg,
    uint8_t *data,
    size_t length)
{
    return main_barometer_classify_error(
        address, sensor_bus_read_bytes(device, reg, length, data));
}

static esp_err_t main_barometer_write_reg(
    i2c_bus_device_handle_t device,
    uint8_t address,
    uint8_t reg,
    uint8_t value)
{
    return main_barometer_classify_error(
        address, sensor_bus_write_bytes(device, reg, 1U, &value));
}

/* Another task owns the shared bus: says nothing about the sensor itself. */
static bool main_barometer_bus_yield_error(esp_err_t err)
{
    return err == SENSOR_BUS_ERR_MUTEX_BUSY ||
           err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
}

static bool main_barometer_retryable_bus_error(esp_err_t err)
{
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_TIMEOUT ||
           err == ESP_ERR_INVALID_STATE;
}

static uint16_t main_barometer_le_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static void main_barometer_aht20_transfer_failed(
    main_barometer_t *barometer,
    esp_err_t err)
{
    if (main_barometer_bus_yield_error(err)) {
        return;
    }
    barometer->aht20_probe_required = true;
    barometer->aht20_initialized = false;
}

static int16_t main_barometer_le_i16(const uint8_t *data)
{
    return (int16_t)main_barometer_le_u16(data);
}

static void main_barometer_parse_calibration(
    main_barometer_bmp280_calibration_t *calibration,
    const uint8_t bytes[24])
{
    calibration->dig_t1 = main_barometer_le_u16(&bytes[0]);
    calibration->dig_t2 = main_barometer_le_i16(&bytes[2]);
    calibration->dig_t3 = main_barometer_le_i16(&bytes[4]);
    calibration->dig_p1 = main_barometer_le_u16(&bytes[6]);
    calibration->dig_p2 = main_barometer_le_i16(&bytes[8]);
    calibration->dig_p3 = main_barometer_le_i16(&bytes[10]);
    calibration->dig_p4 = main_barometer_le_i16(&bytes[12]);
    calibration->dig_p5 = main_barometer_le_i16(&bytes[14]);
    calibration->dig_p6 = main_barometer_le_i16(&bytes[16]);
    calibration->dig_p7 = main_barometer_le_i16(&bytes[18]);
    calibration->dig_p8 = main_barometer_le_i16(&bytes[20]);
    calibration->dig_p9 = main_barometer_le_i16(&bytes[22]);
}

bool main_barometer_bmp280_compensate(
    uint32_t raw_pressure,
    uint32_t raw_temperature,
    const main_barometer_bmp280_calibration_t *calibration,
    uint32_t *pressure_pa,
    int32_t *temperature_mc)
{
    if (calibration == NULL || pressure_pa == NULL || temperature_mc == NULL ||
        raw_pressure > 0xFFFFFU || raw_temperature > 0xFFFFFU ||
        calibration->dig_p1 == 0U) {
        return false;
    }

    const int32_t adc_t = (int32_t)raw_temperature;
    int32_t var1_t =
        (((adc_t >> 3) - ((int32_t)calibration->dig_t1 << 1)) *
         (int32_t)calibration->dig_t2) >> 11;
    int32_t var2_t =
        (((((adc_t >> 4) - (int32_t)calibration->dig_t1) *
           ((adc_t >> 4) - (int32_t)calibration->dig_t1)) >> 12) *
         (int32_t)calibration->dig_t3) >> 14;
    const int32_t t_fine = var1_t + var2_t;
    int32_t temp_centi_c = (t_fine * 5 + 128) >> 8;
    if (temp_centi_c < -4000) {
        temp_centi_c = -4000;
    } else if (temp_centi_c > 8500) {
        temp_centi_c = 8500;
    }

    int64_t var1_p = (int64_t)t_fine - INT64_C(128000);
    int64_t var2_p = var1_p * var1_p * calibration->dig_p6;
    var2_p += (var1_p * calibration->dig_p5) * INT64_C(131072);
    var2_p += (int64_t)calibration->dig_p4 * INT64_C(34359738368);
    var1_p = (var1_p * var1_p * calibration->dig_p3) / INT64_C(256) +
             var1_p * calibration->dig_p2 * INT64_C(4096);
    var1_p = ((INT64_C(140737488355328) + var1_p) * calibration->dig_p1) /
             INT64_C(8589934592);
    if (var1_p == 0) {
        return false;
    }

    int64_t pressure_q24_8 = INT64_C(1048576) - raw_pressure;
    pressure_q24_8 =
        (((pressure_q24_8 * INT64_C(2147483648)) - var2_p) * INT64_C(3125)) /
        var1_p;
    var1_p = ((int64_t)calibration->dig_p9 * (pressure_q24_8 >> 13) *
              (pressure_q24_8 >> 13)) >> 25;
    var2_p = ((int64_t)calibration->dig_p8 * pressure_q24_8) >> 19;
    pressure_q24_8 =
        ((pressure_q24_8 + var1_p + var2_p) >> 8) +
        ((int64_t)calibration->dig_p7 << 4);
    int64_t pressure = (pressure_q24_8 + 128) / 256;
    if (pressure < BMP280_PRESSURE_MIN_PA || pressure > BMP280_PRESSURE_MAX_PA) {
        return false;
    }

    *pressure_pa = (uint32_t)pressure;
    *temperature_mc = temp_centi_c * 10;
    return true;
}

/*
 * Config and mode live in volatile registers: after a power loss or reset the
 * sensor ACKs again but sits in sleep mode returning 0x80000 forever, so this
 * is repeated whenever it comes back, not only on first init.
 */
static esp_err_t main_barometer_arm_bmp280(
    main_barometer_t *barometer,
    i2c_bus_device_handle_t device,
    uint8_t address)
{
    /*
     * T x2, P x16, normal mode; IIR x16 and 62.5 ms standby. The datasheet allows CONFIG writes only in sleep
     * mode (they are ignored in normal mode, which matters on a re-arm), so go to sleep first. A write that
     * is ACKed proves nothing about the register: read the state back and say what the sensor holds.
     */
    barometer->bmp280_conversion_pending = false;
    barometer->bmp280_conversion_seen = false;
    barometer->bmp280_state = MAIN_BMP280_CONFIGURE;
    esp_err_t err;
    for (;;) {
        switch (barometer->bmp280_state) {
        case MAIN_BMP280_CONFIGURE:
            err = main_barometer_write_reg(
                device, address, BMP280_REG_CTRL_MEAS, BMP280_CTRL_SLEEP);
            if (err == ESP_OK) {
                err = main_barometer_write_reg(
                    device, address, BMP280_REG_CONFIG,
                    BMP280_CONFIG_FILTER_X16_STANDBY_62_5_MS);
            }
            if (err == ESP_OK) {
                err = main_barometer_write_reg(
                    device, address, BMP280_REG_CTRL_MEAS,
                    BMP280_CTRL_TEMP_X2_PRESS_X16_NORMAL);
            }
            if (err != ESP_OK) {
                return err;
            }

            barometer->bmp280_state = MAIN_BMP280_VERIFY;
            break;
        case MAIN_BMP280_VERIFY: {
            uint8_t state[3] = {0}; /* STATUS, CTRL_MEAS, CONFIG */
            err = main_barometer_read_regs(device, address, BMP280_REG_STATUS, state, sizeof(state));
            if (err != ESP_OK) {
                return err;
            }
            const bool applied = state[1] == BMP280_CTRL_TEMP_X2_PRESS_X16_NORMAL &&
                (state[2] & 0xFCU) == BMP280_CONFIG_FILTER_X16_STANDBY_62_5_MS;
            ESP_LOGI(TAG,
                "MAIN_BARO_ARM addr=0x%02x wrote ctrl_meas=0x%02x config=0x%02x | read back status=0x%02x ctrl_meas=0x%02x config=0x%02x -> %s",
                (unsigned)address, (unsigned)BMP280_CTRL_TEMP_X2_PRESS_X16_NORMAL,
                (unsigned)BMP280_CONFIG_FILTER_X16_STANDBY_62_5_MS, (unsigned)state[0], (unsigned)state[1],
                (unsigned)state[2], applied ? "APPLIED" : "NOT APPLIED (the sensor ACKs but does not keep the mode)");
            if (applied) {
                barometer->bmp280_state = MAIN_BMP280_MEASURING;
                barometer->bmp280_conversion_seen = (state[0] & 0x08U) != 0U;
            }
            return applied ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        default:
            return ESP_ERR_INVALID_STATE;
        }
    }
}

static esp_err_t main_barometer_init_bmp280(main_barometer_t *barometer)
{
    barometer->bme280 = false;
#if CONFIG_MAIN_BAROMETER_BMP280_ADDRESS == 0x76
    const uint8_t addresses[2] = {BMP280_ADDRESS_76, BMP280_ADDRESS_77};
#elif CONFIG_MAIN_BAROMETER_BMP280_ADDRESS == 0x77
    const uint8_t addresses[2] = {BMP280_ADDRESS_77, BMP280_ADDRESS_76};
#else
#error "CONFIG_MAIN_BAROMETER_BMP280_ADDRESS must be 0x76 or 0x77"
#endif

    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    /* One line per attempt tells what each address did, so a wrong address or part is visible at once. */
    char detail[2][112] = {"not probed", "not probed"};
    for (size_t i = 0U; i < 2U; ++i) {
        barometer->bmp280_state = MAIN_BMP280_PROBE;
        const esp_err_t probe_err = sensor_bus_probe_address(addresses[i]);
        if (probe_err != ESP_OK) {
            (void)snprintf(detail[i], sizeof(detail[i]), "0x%02x NO ANSWER (%s)",
                           (unsigned)addresses[i], esp_err_to_name(probe_err));
            if (probe_err != ESP_ERR_NOT_FOUND || last_error == ESP_ERR_NOT_FOUND) {
                last_error = probe_err;
            }
            continue;
        }

        i2c_bus_device_handle_t candidate = NULL;
        esp_err_t create_err = sensor_bus_device_create(
            barometer->handles.bus, addresses[i], SENSOR_BUS_I2C_SPEED_HZ,
            &candidate);
        if (create_err != ESP_OK) {
            barometer->bme280 = false;
            return create_err;
        }

        uint8_t chip_id = 0U;
        esp_err_t err = main_barometer_read_regs(
            candidate, addresses[i], BMP280_REG_CHIP_ID, &chip_id, 1U);
        if (err == ESP_OK) {
            (void)snprintf(detail[i], sizeof(detail[i]), "0x%02x answers, chip_id=0x%02x (%s)",
                           (unsigned)addresses[i], (unsigned)chip_id,
                           chip_id == BMP280_CHIP_ID ? "BMP280"
                           : chip_id == BME280_CHIP_ID ? "BME280"
                           : "UNKNOWN PART, expected 0x58 BMP280 or 0x60 BME280");
        } else {
            (void)snprintf(detail[i], sizeof(detail[i]), "0x%02x answers, chip id read FAILED (%s)",
                           (unsigned)addresses[i], esp_err_to_name(err));
        }
        if (err == ESP_OK &&
            (chip_id == BMP280_CHIP_ID || chip_id == BME280_CHIP_ID)) {
            barometer->bme280 = (chip_id == BME280_CHIP_ID);
            uint8_t calibration_bytes[24] = {0};
            err = main_barometer_read_regs(
                candidate, addresses[i], BMP280_REG_CALIB,
                calibration_bytes, sizeof(calibration_bytes));
            if (err == ESP_OK) {
                main_barometer_parse_calibration(
                    &barometer->calibration, calibration_bytes);
                if (barometer->calibration.dig_p1 == 0U) {
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            }
            if (err == ESP_OK) {
                err = main_barometer_arm_bmp280(barometer, candidate, addresses[i]);
            }
            if (err == ESP_OK) {
                barometer->bmp280 = candidate;
                barometer->bmp280_pending_polls = 0U;
                barometer->bmp280_pending_count_due_ms = 0U;
                barometer->bmp280_forced = false;
                barometer->reading.bmp280_address = addresses[i];
                barometer->reading.bmp280_present = true;
                barometer->bmp280_initialized = true;
                ESP_LOGI(TAG, "MAIN_BARO_PROBE %s in use | %s | %s",
                         barometer->bme280 ? "BME280" : "BMP280", detail[0], detail[1]);
                return ESP_OK;
            }
        } else if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }

        if (err != ESP_OK) {
            const size_t used = strlen(detail[i]);
            (void)snprintf(detail[i] + used, sizeof(detail[i]) - used, " -> init failed: %s",
                           esp_err_to_name(err));
        }
        if (err != ESP_ERR_NOT_FOUND) {
            last_error = err;
        }
        const esp_err_t delete_err = sensor_bus_device_delete(&candidate);
        if (delete_err != ESP_OK) {
            barometer->bmp280 = candidate;
            barometer->bmp280_state = MAIN_BMP280_RECOVER;
            return delete_err;
        }
    }

    barometer->reading.bmp280_present = false;
    barometer->reading.bmp280_valid = false;
    barometer->bme280 = false;
    ESP_LOGW(TAG, "MAIN_BARO_PROBE BMP280/BME280 NOT USABLE | %s | %s | last_error=%s",
             detail[0], detail[1], esp_err_to_name(last_error));
    return last_error;
}

uint8_t main_barometer_aht20_crc8(const uint8_t *data, uint8_t length)
{
    if (data == NULL) {
        return 0U;
    }
    uint8_t crc = 0xFFU;
    for (uint8_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x80U) != 0U
                ? (uint8_t)((crc << 1U) ^ 0x31U)
                : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static esp_err_t main_barometer_init_aht20(main_barometer_t *barometer)
{
    if (barometer->aht20 == NULL) {
        const esp_err_t create_err = sensor_bus_device_create(
            barometer->handles.bus, AHT20_ADDRESS, SENSOR_BUS_I2C_SPEED_HZ,
            &barometer->aht20);
        if (create_err != ESP_OK) {
            return create_err;
        }
    }

    if (barometer->aht20_probe_required) {
        const esp_err_t probe_err = sensor_bus_probe_address(AHT20_ADDRESS);
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "MAIN_BARO_PROBE AHT20 addr=0x%02x NO ANSWER (%s)", (unsigned)AHT20_ADDRESS,
                     esp_err_to_name(probe_err));
            return probe_err;
        }
    }

    uint8_t status = 0U;
    esp_err_t err = main_barometer_classify_error(
        AHT20_ADDRESS,
        sensor_bus_read_byte(barometer->aht20, NULL_I2C_MEM_ADDR, &status));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAIN_BARO_PROBE AHT20 addr=0x%02x answers but the status read FAILED (%s)",
                 (unsigned)AHT20_ADDRESS, esp_err_to_name(err));
        main_barometer_aht20_transfer_failed(barometer, err);
        return err;
    }
    ESP_LOGI(TAG, "MAIN_BARO_PROBE AHT20 addr=0x%02x answers, status=0x%02x (%s)", (unsigned)AHT20_ADDRESS,
             (unsigned)status, (status & AHT20_STATUS_CALIBRATED) != 0U ? "calibrated" : "not calibrated yet");
    barometer->reading.aht20_present = true;

    if ((status & AHT20_STATUS_CALIBRATED) == 0U) {
        const uint8_t initialize[3] = {AHT20_CMD_INITIALIZE, 0x08U, 0x00U};
        err = main_barometer_classify_error(
            AHT20_ADDRESS,
            sensor_bus_write_bytes(
                barometer->aht20, NULL_I2C_MEM_ADDR,
                sizeof(initialize), initialize));
        if (err != ESP_OK) {
            main_barometer_aht20_transfer_failed(barometer, err);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(AHT20_INIT_DELAY_MS));
        err = main_barometer_classify_error(
            AHT20_ADDRESS,
            sensor_bus_read_byte(barometer->aht20, NULL_I2C_MEM_ADDR, &status));
        if (err != ESP_OK) {
            main_barometer_aht20_transfer_failed(barometer, err);
            return err;
        }
        if ((status & AHT20_STATUS_CALIBRATED) == 0U) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    barometer->aht20_initialized = true;
    barometer->aht20_probe_required = false;
    return ESP_OK;
}

static esp_err_t main_barometer_read_aht20(main_barometer_t *barometer)
{
    if (!barometer->aht20_initialized) {
        const esp_err_t init_err = main_barometer_init_aht20(barometer);
        if (init_err != ESP_OK) {
            barometer->reading.aht20_valid = false;
            return init_err;
        }
    }

    if (barometer->aht20_probe_required) {
        const esp_err_t probe_err = sensor_bus_probe_address(AHT20_ADDRESS);
        if (probe_err != ESP_OK) {
            barometer->reading.aht20_valid = false;
            return probe_err;
        }
        barometer->aht20_probe_required = false;
    }

    const uint8_t trigger[3] = {AHT20_CMD_MEASURE, 0x33U, 0x00U};
    esp_err_t err = main_barometer_classify_error(
        AHT20_ADDRESS,
        sensor_bus_write_bytes(
            barometer->aht20, NULL_I2C_MEM_ADDR, sizeof(trigger), trigger));
    if (err != ESP_OK) {
        main_barometer_aht20_transfer_failed(barometer, err);
        barometer->reading.aht20_valid = false;
        return err;
    }

    uint8_t data[7] = {0};
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)AHT20_TIMEOUT_MS * INT64_C(1000);
    bool ready = false;
    while (esp_timer_get_time() < deadline_us) {
        /* A 2 ms poll is one 10 ms tick at HZ=100; the deadline remains wall time. */
        vTaskDelay((TickType_t)app_ms_to_ticks_min_one(
            AHT20_BUSY_POLL_MS, configTICK_RATE_HZ));
        if (esp_timer_get_time() >= deadline_us) {
            break;
        }
        err = main_barometer_classify_error(
            AHT20_ADDRESS,
            sensor_bus_read_bytes(
                barometer->aht20, NULL_I2C_MEM_ADDR, sizeof(data), data));
        if (err != ESP_OK) {
            main_barometer_aht20_transfer_failed(barometer, err);
            barometer->reading.aht20_valid = false;
            return err;
        }
        if (esp_timer_get_time() >= deadline_us) {
            break;
        }
        if ((data[0] & AHT20_STATUS_BUSY) == 0U) {
            ready = true;
            break;
        }
    }

    if (!ready) {
        barometer->reading.aht20_valid = false;
        return ESP_ERR_TIMEOUT;
    }
    if (main_barometer_aht20_crc8(data, 6U) != data[6]) {
        barometer->reading.aht20_valid = false;
        return ESP_ERR_INVALID_STATE;
    }
    if ((data[0] & AHT20_STATUS_CALIBRATED) == 0U) {
        barometer->reading.aht20_valid = false;
        barometer->aht20_initialized = false;
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t raw_humidity =
        ((uint32_t)data[1] << 12U) | ((uint32_t)data[2] << 4U) |
        ((uint32_t)data[3] >> 4U);
    const uint32_t raw_temperature =
        (((uint32_t)data[3] & 0x0FU) << 16U) | ((uint32_t)data[4] << 8U) |
        (uint32_t)data[5];
    barometer->reading.humidity_mpct =
        (uint32_t)(((uint64_t)raw_humidity * UINT64_C(100000) + 524288U) /
                   UINT64_C(1048576));
    barometer->reading.aht20_temperature_mc =
        (int32_t)(((uint64_t)raw_temperature * UINT64_C(200000) + 524288U) /
                  UINT64_C(1048576)) - 50000;
    barometer->reading.aht20_valid = true;
    return ESP_OK;
}

void main_barometer_init(main_barometer_t *barometer)
{
    if (barometer == NULL) {
        return;
    }
    memset(barometer, 0, sizeof(*barometer));
    barometer->aht20_probe_required = true;
    (void)nrf_ema_filter_init(
        &barometer->altitude_filter, 1000U, -2000000.0, 10000000.0);
    (void)nrf_ema_filter_init(
        &barometer->pressure_filter, 1000U, BMP280_PRESSURE_MIN_PA,
        BMP280_PRESSURE_MAX_PA);
}

void main_barometer_request_polling(void)
{
    atomic_store_explicit(&s_polling_requested, true, memory_order_release);
}

esp_err_t main_barometer_request_height_reference_capture(uint16_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store_explicit(
        &s_height_reference_request_generation, generation, memory_order_release);
    main_barometer_request_polling();
    return ESP_OK;
}

static void main_barometer_height_reference_sensor_invalid(
    main_barometer_t *barometer)
{
    if (barometer->reading.height_reference_generation == 0U ||
        (!barometer->height_reference_capture_active &&
         barometer->reading.height_reference_state != MAIN_BAROMETER_REFERENCE_VALID)) {
        return;
    }
    barometer->height_reference_capture_active = false;
    barometer->height_reference_has_sample = false;
    barometer->reading.height_reference_state = MAIN_BAROMETER_REFERENCE_SENSOR_INVALID;
    barometer->reading.relative_z_mm = 0;
}

/* A conversion that has not finished is not a failure: restart the stability window. */
static void main_barometer_height_reference_pending(main_barometer_t *barometer)
{
    if (barometer->bmp280_pending_polls >= MAIN_BAROMETER_PENDING_INVALID_POLLS ||
        barometer->bmp280_yield_polls >= MAIN_BAROMETER_PENDING_INVALID_POLLS) {
        main_barometer_height_reference_sensor_invalid(barometer);
        return;
    }
    barometer->height_reference_has_sample = false;
}

static void main_barometer_height_reference_update(
    main_barometer_t *barometer,
    uint64_t now_ms)
{
    if (!barometer->height_reference_capture_active) {
        if (barometer->reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID) {
            barometer->reading.relative_z_mm =
                barometer->reading.filtered_altitude_mm -
                barometer->height_reference_altitude_mm;
        }
        return;
    }

    const double pressure_pa = barometer->reading.filtered_pressure_pa;
    if (!barometer->height_reference_has_sample) {
        barometer->height_reference_capture_started_ms = now_ms;
        barometer->height_reference_min_pressure_pa = pressure_pa;
        barometer->height_reference_max_pressure_pa = pressure_pa;
        barometer->height_reference_has_sample = true;
        return;
    }

    if (pressure_pa < barometer->height_reference_min_pressure_pa) {
        barometer->height_reference_min_pressure_pa = pressure_pa;
    }
    if (pressure_pa > barometer->height_reference_max_pressure_pa) {
        barometer->height_reference_max_pressure_pa = pressure_pa;
    }
    if (now_ms < barometer->height_reference_capture_started_ms ||
        now_ms - barometer->height_reference_capture_started_ms <
            MAIN_HEIGHT_REFERENCE_WINDOW_MS) {
        return;
    }

    barometer->height_reference_capture_active = false;
    if (barometer->height_reference_max_pressure_pa -
            barometer->height_reference_min_pressure_pa <=
        MAIN_HEIGHT_REFERENCE_MAX_PRESSURE_RANGE_PA) {
        barometer->height_reference_altitude_mm =
            barometer->reading.filtered_altitude_mm;
        barometer->reading.height_reference_state = MAIN_BAROMETER_REFERENCE_VALID;
        barometer->reading.relative_z_mm = 0;
    } else {
        barometer->reading.height_reference_state =
            MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE;
        barometer->reading.relative_z_mm = 0;
    }
}

static esp_err_t main_barometer_read_bmp280(
    main_barometer_t *barometer,
    uint64_t now_ms)
{
    uint8_t bytes[6] = {0};
    esp_err_t err = ESP_OK;
    const uint8_t address = barometer->reading.bmp280_address;
    if (barometer->bmp280_forced && !barometer->bmp280_conversion_pending) {
        err = main_barometer_write_reg(barometer->bmp280, address, BMP280_REG_CTRL_MEAS,
                                       BMP280_CTRL_TEMP_X2_PRESS_X16_FORCED);
        if (err != ESP_OK) return err;
        uint8_t ctrl = 0U;
        err = main_barometer_read_regs(barometer->bmp280, address, BMP280_REG_CTRL_MEAS, &ctrl, 1U);
        if (err != ESP_OK) return err;
        if (ctrl != BMP280_CTRL_TEMP_X2_PRESS_X16_FORCED) return ESP_ERR_INVALID_STATE;
        barometer->bmp280_conversion_pending = true;
        barometer->bmp280_conversion_seen = true;
        barometer->bmp280_conversion_ready_ms = now_ms + BMP280_FORCED_CONVERSION_MS;
        barometer->bmp280_conversion_deadline_ms = now_ms + BMP280_CONVERSION_TIMEOUT_MS;
        barometer->next_bmp280_poll_ms = barometer->bmp280_conversion_ready_ms;
        return ESP_ERR_NOT_FINISHED;
    }
    if (barometer->bmp280_conversion_pending && now_ms > barometer->bmp280_conversion_deadline_ms)
        return ESP_ERR_TIMEOUT;
    if (barometer->bmp280_forced && now_ms < barometer->bmp280_conversion_ready_ms)
        return ESP_ERR_NOT_FINISHED;
    uint8_t state[3] = {0};
    err = main_barometer_read_regs(barometer->bmp280, address, BMP280_REG_STATUS, state, sizeof(state));
    if (err != ESP_OK) return err;
    const uint8_t expected_ctrl = barometer->bmp280_forced ? 0x54U : 0x57U;
    if ((state[1] & 0xFCU) != 0x54U || (state[2] & 0xFCU) != 0x30U) {
        barometer->bmp280_conversion_seen = false;
        return ESP_ERR_NOT_FINISHED;
    }
    if ((state[0] & 0x09U) != 0U) {
        if (!barometer->bmp280_conversion_pending) {
            barometer->bmp280_conversion_pending = true;
            barometer->bmp280_conversion_deadline_ms = now_ms + BMP280_CONVERSION_TIMEOUT_MS;
        }
        if (now_ms >= barometer->bmp280_conversion_deadline_ms) return ESP_ERR_TIMEOUT;
        if ((state[0] & 0x08U) != 0U) barometer->bmp280_conversion_seen = true;
        barometer->next_bmp280_poll_ms = now_ms + 10U;
        return ESP_ERR_NOT_FINISHED;
    }
    if (state[1] != expected_ctrl || !barometer->bmp280_conversion_seen) {
        if (barometer->bmp280_conversion_pending && now_ms >= barometer->bmp280_conversion_deadline_ms)
            return ESP_ERR_TIMEOUT;
        if (!barometer->bmp280_forced) barometer->next_bmp280_poll_ms = now_ms + 20U;
        return ESP_ERR_NOT_FINISHED;
    }
    barometer->bmp280_conversion_pending = false;
    err = main_barometer_read_regs(
        barometer->bmp280, barometer->reading.bmp280_address,
        BMP280_REG_DATA, bytes, sizeof(bytes));
    if (err != ESP_OK) {
        barometer->reading.bmp280_valid = false;
        return err;
    }

    const uint32_t raw_pressure =
        ((uint32_t)bytes[0] << 12U) | ((uint32_t)bytes[1] << 4U) |
        ((uint32_t)bytes[2] >> 4U);
    const uint32_t raw_temperature =
        ((uint32_t)bytes[3] << 12U) | ((uint32_t)bytes[4] << 4U) |
        ((uint32_t)bytes[5] >> 4U);
    uint32_t pressure_pa = 0U;
    int32_t temperature_mc = 0;
    if (raw_pressure == 0x80000U || raw_temperature == 0x80000U) {
        if (now_ms >= barometer->bmp280_pending_count_due_ms &&
            ((unsigned)barometer->bmp280_pending_polls + 1U) % MAIN_BAROMETER_PENDING_REARM_POLLS == 0U) {
            ESP_LOGW(TAG, "MAIN_BARO addr=0x%02x data still at the reset value (%02x %02x %02x %02x %02x %02x) | pending_poll=%u",
                     (unsigned)barometer->reading.bmp280_address,
                     (unsigned)bytes[0], (unsigned)bytes[1], (unsigned)bytes[2],
                     (unsigned)bytes[3], (unsigned)bytes[4], (unsigned)bytes[5],
                     (unsigned)barometer->bmp280_pending_polls + 1U);
        }
        /* Reset value of the data registers: no conversion has finished yet.
         * Not a bus error; the next poll reads it again without backoff. */
        barometer->reading.bmp280_valid = false;
        return ESP_ERR_NOT_FINISHED;
    }
    if (!main_barometer_bmp280_compensate(
            raw_pressure, raw_temperature, &barometer->calibration,
            &pressure_pa, &temperature_mc)) {
        barometer->reading.bmp280_valid = false;
        return ESP_ERR_INVALID_STATE;
    }

    double filtered_altitude_mm = 0.0;
    double filtered_pressure_pa = 0.0;
    const int32_t altitude_mm = nrf_baro_pressure_to_altitude_mm(pressure_pa);
    if (nrf_ema_filter_update(
            &barometer->pressure_filter, pressure_pa,
            now_ms, &filtered_pressure_pa) != NRF_EMA_FILTER_UPDATED ||
        nrf_ema_filter_update(
            &barometer->altitude_filter, altitude_mm,
            now_ms, &filtered_altitude_mm) !=
        NRF_EMA_FILTER_UPDATED) {
        barometer->reading.bmp280_valid = false;
        return ESP_ERR_INVALID_STATE;
    }

    barometer->reading.pressure_pa = pressure_pa;
    barometer->reading.temperature_mc = temperature_mc;
    barometer->reading.filtered_pressure_pa = filtered_pressure_pa;
    barometer->reading.filtered_altitude_mm =
        (int32_t)(filtered_altitude_mm >= 0.0
                      ? filtered_altitude_mm + 0.5
                      : filtered_altitude_mm - 0.5);
    barometer->reading.bmp280_valid = true;
    if (!barometer->bmp280_forced) {
        /* Preserve a next conversion observed after reading this sample. */
        uint8_t status = 0U;
        err = main_barometer_read_regs(barometer->bmp280, address, BMP280_REG_STATUS, &status, 1U);
        if (err != ESP_OK) return err;
        barometer->bmp280_conversion_seen = (status & 0x08U) != 0U;
    }
    return ESP_OK;
}

esp_err_t main_barometer_poll(main_barometer_t *barometer, uint64_t now_ms)
{
    if (barometer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_polling_requested, memory_order_acquire)) {
        return ESP_OK;
    }
    const uint16_t requested_generation = atomic_exchange_explicit(
        &s_height_reference_request_generation, 0U, memory_order_acq_rel);
    if (requested_generation != 0U) {
        barometer->reading.height_reference_generation = requested_generation;
        barometer->reading.height_reference_state =
            MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED;
        barometer->reading.relative_z_mm = 0;
        barometer->height_reference_capture_active = true;
        barometer->height_reference_has_sample = false;
    }
    if (barometer->handles.bus == NULL) {
        const esp_err_t bus_err = sensor_bus_get_handles(&barometer->handles);
        if (bus_err != ESP_OK) {
            main_barometer_height_reference_sensor_invalid(barometer);
            return bus_err;
        }
    }

    esp_err_t result = ESP_OK;
    if (now_ms >= barometer->next_bmp280_poll_ms) {
        barometer->next_bmp280_poll_ms = now_ms + BMP280_POLL_PERIOD_MS;
        esp_err_t bmp_err = ESP_OK;
        bool bmp_retry_deferred = false;
        bool bmp_just_initialized = false;
        switch (barometer->bmp280_state) {
        case MAIN_BMP280_RECOVER:
            if (now_ms < barometer->bmp280_retry_after_ms) {
                bmp_err = barometer->bmp280_retry_error;
                bmp_retry_deferred = true;
                break;
            }
            if (barometer->bmp280 != NULL) {
                bmp_err = sensor_bus_device_delete(&barometer->bmp280);
                if (bmp_err != ESP_OK) {
                    break;
                }
            }
            barometer->bmp280_initialized = false;
            barometer->bmp280_state = MAIN_BMP280_PROBE;
            /* fall through */
        case MAIN_BMP280_PROBE:
            bmp_err = main_barometer_init_bmp280(barometer);
            /* The first conversion finishes before the next poll. */
            bmp_just_initialized = (bmp_err == ESP_OK);
            break;
        case MAIN_BMP280_MEASURING:
            bmp_err = main_barometer_read_bmp280(barometer, now_ms);
            break;
        default:
            bmp_err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (bmp_err == ESP_OK) {
            barometer->bmp280_retry_after_ms = 0U;
            barometer->bmp280_retry_error = ESP_OK;
            barometer->bmp280_yield_polls = 0U;
            if (!bmp_just_initialized) {
                barometer->bmp280_pending_polls = 0U;
                barometer->bmp280_pending_count_due_ms = 0U;
                main_barometer_height_reference_update(barometer, now_ms);
            }
        } else {
            barometer->reading.bmp280_valid = false;
            if (bmp_err == ESP_ERR_NOT_FINISHED && now_ms >= barometer->bmp280_pending_count_due_ms) {
                barometer->bmp280_pending_count_due_ms = now_ms + BMP280_POLL_PERIOD_MS;
                if (barometer->bmp280_pending_polls < UINT8_MAX) {
                    ++barometer->bmp280_pending_polls;
                }
                if (barometer->bmp280_pending_polls == MAIN_BAROMETER_FORCED_FALLBACK_POLLS &&
                    !barometer->bmp280_forced) {
                    barometer->bmp280_forced = true;
                    ESP_LOGW(TAG, "MAIN_BARO normal mode produced no data for %u polls: trying forced conversions",
                             (unsigned)barometer->bmp280_pending_polls);
                }
                if (!barometer->bmp280_forced && barometer->bmp280_pending_polls %
                        MAIN_BAROMETER_PENDING_REARM_POLLS == 0U) {
                    ESP_LOGW(TAG, "MAIN_BARO addr=0x%02x no fresh conversion after %u polls: re-arming",
                             (unsigned)barometer->reading.bmp280_address, (unsigned)barometer->bmp280_pending_polls);
                    const esp_err_t rearm_err = main_barometer_arm_bmp280(
                        barometer, barometer->bmp280, barometer->reading.bmp280_address);
                    if (rearm_err != ESP_OK) {
                        bmp_err = rearm_err;
                    }
                }
            }
            const bool bus_yield = main_barometer_bus_yield_error(bmp_err);
            if (bus_yield) {
                if (barometer->bmp280_yield_polls < UINT8_MAX) {
                    ++barometer->bmp280_yield_polls;
                }
            } else {
                barometer->bmp280_yield_polls = 0U;
            }
            if (bmp_err != ESP_ERR_NOT_FINISHED && !bus_yield) {
                /* A bus-level failure: probe again before the next read. */
                barometer->bmp280_state = MAIN_BMP280_RECOVER;
                barometer->bmp280_conversion_pending = false;
                barometer->bmp280_conversion_seen = false;
                if (!bmp_retry_deferred) {
                    barometer->bmp280_pending_polls = 0U;
                }
            }
            if (bus_yield && barometer->bmp280_state != MAIN_BMP280_MEASURING) {
                barometer->bmp280_state = barometer->bmp280 != NULL
                    ? MAIN_BMP280_RECOVER : MAIN_BMP280_PROBE;
            }
            if (!bmp_retry_deferred) {
                barometer->bmp280_retry_error = bmp_err;
                if (main_barometer_retryable_bus_error(bmp_err)) {
                    barometer->bmp280_retry_after_ms =
                        now_ms + MAIN_BAROMETER_RETRY_BACKOFF_MS;
                } else {
                    barometer->bmp280_retry_after_ms = now_ms;
                }
            }
            if (bmp_err == ESP_ERR_NOT_FINISHED || bus_yield) {
                const bool recent_sample = barometer->pressure_filter.initialized &&
                    now_ms >= barometer->pressure_filter.timestamp_ms &&
                    now_ms - barometer->pressure_filter.timestamp_ms <= BMP280_CONVERSION_TIMEOUT_MS;
                if ((!barometer->bmp280_conversion_pending && !recent_sample) ||
                    barometer->bmp280_pending_polls >= MAIN_BAROMETER_PENDING_INVALID_POLLS || bus_yield)
                    main_barometer_height_reference_pending(barometer);
            } else {
                main_barometer_height_reference_sensor_invalid(barometer);
            }
        }
        if (result == ESP_OK) {
            result = bmp_err;
        }
    }

    if (!barometer->bme280 &&
        (!barometer->aht20_attempted ||
         (now_ms >= barometer->last_aht20_attempt_ms &&
          now_ms - barometer->last_aht20_attempt_ms >= AHT20_POLL_PERIOD_MS))) {
        barometer->aht20_attempted = true;
        barometer->last_aht20_attempt_ms = now_ms;
        esp_err_t aht_err;
        bool aht_retry_deferred = false;
        if (barometer->aht20_probe_required &&
            now_ms < barometer->aht20_retry_after_ms) {
            aht_err = barometer->aht20_retry_error;
            aht_retry_deferred = true;
        } else {
            aht_err = main_barometer_read_aht20(barometer);
        }
        if (aht_err == ESP_OK) {
            barometer->aht20_retry_after_ms = 0U;
            barometer->aht20_retry_error = ESP_OK;
        } else if (barometer->aht20_probe_required && !aht_retry_deferred) {
            barometer->aht20_retry_error = aht_err;
            if (main_barometer_retryable_bus_error(aht_err)) {
                barometer->aht20_retry_after_ms =
                    now_ms + MAIN_BAROMETER_RETRY_BACKOFF_MS;
            } else {
                barometer->aht20_retry_after_ms = now_ms;
            }
        }
        if (result == ESP_OK) {
            result = aht_err;
        }
    }
    return result;
}

void main_barometer_close(main_barometer_t *barometer)
{
    if (barometer == NULL) {
        return;
    }
    if (barometer->bmp280 != NULL) {
        (void)sensor_bus_device_delete(&barometer->bmp280);
    }
    if (barometer->aht20 != NULL) {
        (void)sensor_bus_device_delete(&barometer->aht20);
    }
    memset(&barometer->handles, 0, sizeof(barometer->handles));
    barometer->bmp280_initialized = false;
    barometer->bmp280_state = MAIN_BMP280_PROBE;
    barometer->bmp280_pending_polls = 0U;
    barometer->bmp280_pending_count_due_ms = 0U;
    barometer->bmp280_yield_polls = 0U;
    barometer->bmp280_forced = false;
    barometer->bmp280_conversion_pending = false;
    barometer->bmp280_conversion_seen = false;
    barometer->reading.bmp280_valid = false;
    barometer->aht20_initialized = false;
    barometer->aht20_probe_required = true;
    barometer->bmp280_retry_after_ms = 0U;
    barometer->aht20_retry_after_ms = 0U;
    barometer->bmp280_retry_error = ESP_OK;
    barometer->aht20_retry_error = ESP_OK;
}

void main_barometer_get_reading(
    const main_barometer_t *barometer,
    main_barometer_reading_t *reading)
{
    if (barometer != NULL && reading != NULL) {
        *reading = barometer->reading;
    }
}
