#include "nrf_bme280.h"

#include <stddef.h>
#include <string.h>

/* Bosch BME280 register map and the fixed REMOTE measurement profile. */
enum {
    BME280_REG_CHIP_ID = 0xD0U,
    BME280_REG_RESET = 0xE0U,
    BME280_REG_TEMP_PRESS_CALIB = 0x88U,
    BME280_REG_HUMIDITY_CALIB = 0xE1U,
    BME280_REG_CTRL_HUM = 0xF2U,
    BME280_REG_STATUS = 0xF3U,
    BME280_REG_CTRL_MEAS = 0xF4U,
    BME280_REG_CONFIG = 0xF5U,
    BME280_REG_DATA = 0xF7U,

    BME280_SOFT_RESET = 0xB6U,
    BME280_STATUS_IM_UPDATE = 0x01U,
    BME280_STATUS_MEASURING = 0x08U,

    /* Bosch indoor-navigation style profile: H x1, T x2, P x16, IIR x16. */
    BME280_CTRL_HUM_X1 = 0x01U,
    BME280_CTRL_MEAS_SLEEP = 0x54U,
    BME280_CTRL_MEAS_FORCED = 0x55U,
    BME280_CONFIG_FILTER_X16 = 0x10U,

    BMP280_CALIB_BYTES = 24U,
    BMP280_DATA_BYTES = 6U,

    BME280_STARTUP_DELAY_MS = 2U,
    BME280_NVM_POLL_DELAY_MS = 2U,
    BME280_NVM_POLL_ATTEMPTS = 10U,
    /* 46.1 ms from Bosch's measurement-time formula; round up to 47 ms. */
    BME280_MEASUREMENT_DELAY_MS = 47U,
    BME280_MEASUREMENT_POLL_DELAY_MS = 2U,
    BME280_MEASUREMENT_POLL_ATTEMPTS = 5U,
};

static nrf_bme280_result_t bme280_read_regs_at(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    uint8_t reg,
    uint8_t *data,
    size_t data_len)
{
    return nrf_sensor_bus_write_read(
               bus,
               address,
               &reg,
               1U,
               data,
               data_len) == NRF_SENSOR_BUS_OK
               ? NRF_BME280_OK
               : NRF_BME280_ERR_BUS;
}

static nrf_bme280_result_t bme280_read_regs(
    const nrf_bme280_t *device,
    uint8_t reg,
    uint8_t *data,
    size_t data_len)
{
    return bme280_read_regs_at(
        &device->bus,
        device->address,
        reg,
        data,
        data_len);
}

static nrf_bme280_result_t bme280_write_reg(
    nrf_bme280_t *device,
    uint8_t reg,
    uint8_t value)
{
    const uint8_t packet[2] = {reg, value};
    return nrf_sensor_bus_write(
               &device->bus,
               device->address,
               packet,
               sizeof(packet)) == NRF_SENSOR_BUS_OK
               ? NRF_BME280_OK
               : NRF_BME280_ERR_BUS;
}

static uint16_t bme280_read_le_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static int16_t bme280_read_le_i16(const uint8_t *data)
{
    return (int16_t)bme280_read_le_u16(data);
}

static void bme280_parse_calibration(
    nrf_bme280_calibration_t *calibration,
    const uint8_t temp_press[26],
    const uint8_t humidity[7])
{
    calibration->dig_t1 = bme280_read_le_u16(&temp_press[0]);
    calibration->dig_t2 = bme280_read_le_i16(&temp_press[2]);
    calibration->dig_t3 = bme280_read_le_i16(&temp_press[4]);
    calibration->dig_p1 = bme280_read_le_u16(&temp_press[6]);
    calibration->dig_p2 = bme280_read_le_i16(&temp_press[8]);
    calibration->dig_p3 = bme280_read_le_i16(&temp_press[10]);
    calibration->dig_p4 = bme280_read_le_i16(&temp_press[12]);
    calibration->dig_p5 = bme280_read_le_i16(&temp_press[14]);
    calibration->dig_p6 = bme280_read_le_i16(&temp_press[16]);
    calibration->dig_p7 = bme280_read_le_i16(&temp_press[18]);
    calibration->dig_p8 = bme280_read_le_i16(&temp_press[20]);
    calibration->dig_p9 = bme280_read_le_i16(&temp_press[22]);
    calibration->dig_h1 = temp_press[25];

    calibration->dig_h2 = bme280_read_le_i16(&humidity[0]);
    calibration->dig_h3 = humidity[2];
    calibration->dig_h4 =
        (int16_t)(((int16_t)(int8_t)humidity[3] * 16) |
                  (int16_t)(humidity[4] & 0x0FU));
    calibration->dig_h5 =
        (int16_t)(((int16_t)(int8_t)humidity[5] * 16) |
                  (int16_t)(humidity[4] >> 4U));
    calibration->dig_h6 = (int8_t)humidity[6];
}

static int32_t bme280_compensate_temperature_centi_c(
    uint32_t raw_temperature,
    const nrf_bme280_calibration_t *calibration,
    int32_t *t_fine)
{
    int32_t var1 =
        (int32_t)(raw_temperature / 8U) - (int32_t)calibration->dig_t1 * 2;
    var1 = (var1 * (int32_t)calibration->dig_t2) / 2048;

    int32_t var2 =
        (int32_t)(raw_temperature / 16U) - (int32_t)calibration->dig_t1;
    var2 = (((var2 * var2) / 4096) * (int32_t)calibration->dig_t3) /
           16384;

    *t_fine = var1 + var2;
    int32_t temperature = (*t_fine * 5 + 128) / 256;
    if (temperature < -4000) {
        temperature = -4000;
    } else if (temperature > 8500) {
        temperature = 8500;
    }
    return temperature;
}

static uint32_t bme280_compensate_pressure_x100_pa(
    uint32_t raw_pressure,
    int32_t t_fine,
    const nrf_bme280_calibration_t *calibration)
{
    int64_t var1 = (int64_t)t_fine - INT64_C(128000);
    int64_t var2 = var1 * var1 * (int64_t)calibration->dig_p6;
    var2 += (var1 * (int64_t)calibration->dig_p5) * INT64_C(131072);
    var2 += (int64_t)calibration->dig_p4 * INT64_C(34359738368);

    var1 = (var1 * var1 * (int64_t)calibration->dig_p3) / INT64_C(256) +
           var1 * (int64_t)calibration->dig_p2 * INT64_C(4096);
    const int64_t var3 = INT64_C(140737488355328);
    var1 = ((var3 + var1) * (int64_t)calibration->dig_p1) /
           INT64_C(8589934592);

    uint32_t pressure = 3000000U;
    if (var1 != 0) {
        int64_t var4 = INT64_C(1048576) - (int64_t)raw_pressure;
        var4 = (((var4 * INT64_C(2147483648)) - var2) * INT64_C(3125)) /
               var1;
        var1 = ((int64_t)calibration->dig_p9 * (var4 / INT64_C(8192)) *
                (var4 / INT64_C(8192))) /
               INT64_C(33554432);
        var2 = ((int64_t)calibration->dig_p8 * var4) / INT64_C(524288);
        var4 = ((var4 + var1 + var2) / INT64_C(256)) +
               (int64_t)calibration->dig_p7 * INT64_C(16);

        int64_t pressure_signed =
            ((var4 / INT64_C(2)) * INT64_C(100)) / INT64_C(128);
        if (pressure_signed < INT64_C(3000000)) {
            pressure_signed = INT64_C(3000000);
        } else if (pressure_signed > INT64_C(11000000)) {
            pressure_signed = INT64_C(11000000);
        }
        pressure = (uint32_t)pressure_signed;
    }
    return pressure;
}

static uint32_t bme280_compensate_humidity_x1024_percent(
    uint32_t raw_humidity,
    int32_t t_fine,
    const nrf_bme280_calibration_t *calibration)
{
    int64_t var1 = (int64_t)t_fine - INT64_C(76800);
    int64_t var2 = (int64_t)raw_humidity * INT64_C(16384);
    int64_t var3 = (int64_t)calibration->dig_h4 * INT64_C(1048576);
    int64_t var4 = (int64_t)calibration->dig_h5 * var1;
    int64_t var5 = ((var2 - var3 - var4) + INT64_C(16384)) /
                   INT64_C(32768);

    var2 = (var1 * (int64_t)calibration->dig_h6) / INT64_C(1024);
    var3 = (var1 * (int64_t)calibration->dig_h3) / INT64_C(2048);
    var4 = (var2 * (var3 + INT64_C(32768))) / INT64_C(1024) +
           INT64_C(2097152);
    var2 = (var4 * (int64_t)calibration->dig_h2 + INT64_C(8192)) /
           INT64_C(16384);
    var3 = var5 * var2;
    var4 = ((var3 / INT64_C(32768)) * (var3 / INT64_C(32768))) /
           INT64_C(128);
    var5 = var3 - (var4 * (int64_t)calibration->dig_h1) / INT64_C(16);

    if (var5 < 0) {
        var5 = 0;
    } else if (var5 > INT64_C(419430400)) {
        var5 = INT64_C(419430400);
    }

    uint32_t humidity = (uint32_t)(var5 / INT64_C(4096));
    if (humidity > 102400U) {
        humidity = 102400U;
    }
    return humidity;
}

static nrf_bme280_result_t bme280_probe(nrf_bme280_t *device)
{
    const uint8_t addresses[2] = {
        NRF_BME280_ADDRESS_PRIMARY,
        NRF_BME280_ADDRESS_SECONDARY,
    };
    bool saw_response = false;
    bool found = false;

    /* Probe both addresses even after a hit so the caller can report what is on the bus. */
    for (size_t index = 0U; index < 2U; ++index) {
        uint8_t chip_id = 0U;
        const uint8_t chip_id_reg = BME280_REG_CHIP_ID;
        const nrf_sensor_bus_result_t bus_result = nrf_sensor_bus_write_read(
            &device->bus,
            addresses[index],
            &chip_id_reg,
            1U,
            &chip_id,
            1U);
        const bool acked = bus_result == NRF_SENSOR_BUS_OK;
        device->probe[index] = (nrf_bme280_probe_t){
            .address = addresses[index],
            .acked = acked,
            .chip_id = acked ? chip_id : 0U,
            .bus_result = bus_result,
        };
        if (!acked) {
            continue;
        }

        saw_response = true;
        const bool is_bme = chip_id == NRF_BME280_CHIP_ID;
        const bool is_bmp = chip_id == NRF_BMP280_CHIP_ID ||
                            chip_id == NRF_BMP280_CHIP_ID_SAMPLE_A ||
                            chip_id == NRF_BMP280_CHIP_ID_SAMPLE_B;
        if (!found && (is_bme || is_bmp)) {
            device->address = addresses[index];
            device->has_humidity = is_bme;
            found = true;
        }
    }

    if (found) {
        return NRF_BME280_OK;
    }
    return saw_response ? NRF_BME280_ERR_CHIP_ID : NRF_BME280_ERR_BUS;
}

static nrf_bme280_result_t bme280_wait_for_nvm(nrf_bme280_t *device)
{
    for (uint32_t attempt = 0U; attempt < BME280_NVM_POLL_ATTEMPTS; ++attempt) {
        uint8_t status = 0U;
        const nrf_bme280_result_t result = bme280_read_regs(
            device,
            BME280_REG_STATUS,
            &status,
            1U);
        if (result != NRF_BME280_OK) {
            return result;
        }
        if ((status & BME280_STATUS_IM_UPDATE) == 0U) {
            return NRF_BME280_OK;
        }
        if (attempt + 1U < BME280_NVM_POLL_ATTEMPTS) {
            device->delay_ms(device->delay_context, BME280_NVM_POLL_DELAY_MS);
        }
    }
    return NRF_BME280_ERR_NVM;
}

static nrf_bme280_result_t bme280_configure_after_nvm(nrf_bme280_t *device)
{
    uint8_t temp_press_calib[26] = {0};
    uint8_t humidity_calib[7] = {0};
    nrf_bme280_result_t result = bme280_read_regs(device, BME280_REG_TEMP_PRESS_CALIB,
        temp_press_calib, device->has_humidity ? sizeof(temp_press_calib) : BMP280_CALIB_BYTES);
    if (result != NRF_BME280_OK) return result;
    if (device->has_humidity) {
        result = bme280_read_regs(device, BME280_REG_HUMIDITY_CALIB, humidity_calib, sizeof(humidity_calib));
        if (result != NRF_BME280_OK) return result;
    }
    bme280_parse_calibration(&device->calibration, temp_press_calib, humidity_calib);
    if (device->calibration.dig_p1 == 0U) return NRF_BME280_ERR_CALIBRATION;
    if (device->has_humidity) {
        result = bme280_write_reg(device, BME280_REG_CTRL_HUM, BME280_CTRL_HUM_X1);
        if (result != NRF_BME280_OK) return result;
    }
    result = bme280_write_reg(device, BME280_REG_CONFIG, BME280_CONFIG_FILTER_X16);
    if (result != NRF_BME280_OK) return result;
    return bme280_write_reg(device, BME280_REG_CTRL_MEAS, BME280_CTRL_MEAS_SLEEP);
}

static nrf_bme280_result_t bme280_prepare_step(nrf_bme280_t *device)
{
    uint8_t config[4] = {0};
    nrf_bme280_result_t result = NRF_BME280_OK;
    switch (device->state) {
    case NRF_BME280_PROBE:
        result = bme280_probe(device);
        if (result != NRF_BME280_OK) {
            goto failed;
        }

        device->state = NRF_BME280_CONFIGURE;
        break;
    case NRF_BME280_CONFIGURE: {
        result = bme280_write_reg(device, BME280_REG_RESET, BME280_SOFT_RESET);
        if (result != NRF_BME280_OK) {
            goto failed;
        }
        device->delay_ms(device->delay_context, BME280_STARTUP_DELAY_MS);

        result = bme280_wait_for_nvm(device);
        if (result != NRF_BME280_OK) {
            goto failed;
        }

        result = bme280_configure_after_nvm(device);
        if (result != NRF_BME280_OK) {
            goto failed;
        }

        device->state = NRF_BME280_VERIFY;
        break;
    }
    case NRF_BME280_VERIFY:
        if (device->has_humidity) {
            result = bme280_read_regs(
                device,
                BME280_REG_CTRL_HUM,
                config,
                sizeof(config));
            if (result != NRF_BME280_OK) {
                goto failed;
            }
            if ((config[0] & 0x07U) != BME280_CTRL_HUM_X1 ||
                (config[2] & 0xFCU) != BME280_CTRL_MEAS_SLEEP ||
                (config[2] & 0x03U) != 0U ||
                (config[3] & 0x1CU) != BME280_CONFIG_FILTER_X16) {
                result = NRF_BME280_ERR_CONFIG;
                goto failed;
            }
        } else {
            /* STATUS, CTRL_MEAS, CONFIG: no CTRL_HUM register on a BMP280. */
            result = bme280_read_regs(
                device,
                BME280_REG_STATUS,
                &config[1],
                3U);
            if (result != NRF_BME280_OK) {
                goto failed;
            }
            if ((config[2] & 0xFCU) != BME280_CTRL_MEAS_SLEEP ||
                (config[2] & 0x03U) != 0U ||
                (config[3] & 0x1CU) != BME280_CONFIG_FILTER_X16) {
                result = NRF_BME280_ERR_CONFIG;
                goto failed;
            }
        }

        device->initialized = true;
        device->state = NRF_BME280_MEASURING;
        break;
    default:
        return NRF_BME280_ERR_NOT_INITIALIZED;
    }
    return NRF_BME280_OK;

failed:
    device->initialized = false;
    device->state = NRF_BME280_RECOVER;
    return result;
}

nrf_bme280_result_t nrf_bme280_init_observed(
    nrf_bme280_t *device,
    const nrf_sensor_bus_t *bus,
    nrf_bme280_delay_ms_fn delay_ms,
    void *delay_context,
    nrf_bme280_state_observer_fn observer,
    void *observer_context)
{
    if (device == NULL || bus == NULL || bus->transfer == NULL || delay_ms == NULL) {
        return NRF_BME280_ERR_ARGUMENT;
    }
    *device = (nrf_bme280_t){
        .bus = *bus,
        .delay_ms = delay_ms,
        .delay_context = delay_context,
        .state = NRF_BME280_PROBE,
    };
    if (observer != NULL) observer(observer_context, device->state);
    while (device->state != NRF_BME280_MEASURING) {
        const nrf_bme280_result_t result = bme280_prepare_step(device);
        if (observer != NULL) observer(observer_context, device->state);
        if (result != NRF_BME280_OK) {
            return result;
        }
    }
    return NRF_BME280_OK;
}

nrf_bme280_result_t nrf_bme280_init(
    nrf_bme280_t *device, const nrf_sensor_bus_t *bus,
    nrf_bme280_delay_ms_fn delay_ms, void *delay_context)
{
    return nrf_bme280_init_observed(device, bus, delay_ms, delay_context, NULL, NULL);
}

nrf_bme280_result_t nrf_bme280_begin_async(
    nrf_bme280_t *device, const nrf_sensor_bus_t *bus,
    nrf_bme280_state_observer_fn observer, void *observer_context)
{
    if (device == NULL || bus == NULL || bus->transfer == NULL) return NRF_BME280_ERR_ARGUMENT;
    *device = (nrf_bme280_t){.bus = *bus, .state = NRF_BME280_PROBE,
                              .observer = observer, .observer_context = observer_context};
    if (observer != NULL) observer(observer_context, device->state);
    return NRF_BME280_PENDING;
}

nrf_bme280_result_t nrf_bme280_step_async(nrf_bme280_t *device, uint64_t now_ms)
{
    nrf_bme280_result_t result;
    if (device == NULL) return NRF_BME280_ERR_ARGUMENT;
    if (device->state == NRF_BME280_MEASURING) return NRF_BME280_OK;
    if (device->state == NRF_BME280_RECOVER) return NRF_BME280_ERR_NOT_INITIALIZED;
    if (now_ms < device->prepare_due_ms) return NRF_BME280_PENDING;
    if (device->state == NRF_BME280_PROBE) {
        result = bme280_probe(device);
        if (result != NRF_BME280_OK) goto failed_async;
        device->state = NRF_BME280_CONFIGURE;
    } else if (device->state == NRF_BME280_CONFIGURE && device->prepare_phase == 0U) {
        result = bme280_write_reg(device, BME280_REG_RESET, BME280_SOFT_RESET);
        if (result != NRF_BME280_OK) goto failed_async;
        device->prepare_phase = 1U;
        device->prepare_due_ms = now_ms > UINT64_MAX - BME280_STARTUP_DELAY_MS
            ? UINT64_MAX : now_ms + BME280_STARTUP_DELAY_MS;
        return NRF_BME280_PENDING;
    } else if (device->state == NRF_BME280_CONFIGURE) {
        uint8_t status = 0U;
        result = bme280_read_regs(device, BME280_REG_STATUS, &status, 1U);
        if (result != NRF_BME280_OK) goto failed_async;
        if ((status & BME280_STATUS_IM_UPDATE) != 0U) {
            if (++device->nvm_polls >= BME280_NVM_POLL_ATTEMPTS) {
                result = NRF_BME280_ERR_NVM; goto failed_async;
            }
            device->prepare_due_ms = now_ms > UINT64_MAX - BME280_NVM_POLL_DELAY_MS
                ? UINT64_MAX : now_ms + BME280_NVM_POLL_DELAY_MS;
            return NRF_BME280_PENDING;
        }
        result = bme280_configure_after_nvm(device);
        if (result != NRF_BME280_OK) goto failed_async;
        device->state = NRF_BME280_VERIFY;
    } else if (device->state == NRF_BME280_VERIFY) {
        result = bme280_prepare_step(device);
        if (result != NRF_BME280_OK) goto failed_async;
    } else {
        return NRF_BME280_ERR_NOT_INITIALIZED;
    }
    if (device->observer != NULL) device->observer(device->observer_context, device->state);
    return device->state == NRF_BME280_MEASURING ? NRF_BME280_OK : NRF_BME280_PENDING;

failed_async:
    device->initialized = false;
    device->state = NRF_BME280_RECOVER;
    if (device->observer != NULL) device->observer(device->observer_context, device->state);
    return result;
}

nrf_bme280_result_t nrf_bme280_start_conversion(nrf_bme280_t *device, uint64_t now_ms)
{
    if (device == NULL) return NRF_BME280_ERR_ARGUMENT;
    if (!device->initialized || device->state != NRF_BME280_MEASURING)
        return NRF_BME280_ERR_NOT_INITIALIZED;
    if (device->conversion_pending) return NRF_BME280_PENDING;
    if (now_ms > UINT64_MAX - BME280_MEASUREMENT_DELAY_MS -
                     (BME280_MEASUREMENT_POLL_ATTEMPTS - 1U) * BME280_MEASUREMENT_POLL_DELAY_MS)
        return NRF_BME280_ERR_ARGUMENT;
    /*
     * CTRL_HUM and CONFIG are volatile. After a power loss the sensor ACKs
     * again with humidity oversampling off, so every conversion would skip
     * humidity (0x8000) forever. Rewrite them (the sensor sleeps between forced
     * conversions, so CONFIG is writable); CTRL_MEAS below latches CTRL_HUM.
     */
    nrf_bme280_result_t result = NRF_BME280_OK;
    if (device->has_humidity) {
        result = bme280_write_reg(
            device,
            BME280_REG_CTRL_HUM,
            BME280_CTRL_HUM_X1);
        if (result != NRF_BME280_OK) {
            return result;
        }
    }
    result = bme280_write_reg(
        device,
        BME280_REG_CONFIG,
        BME280_CONFIG_FILTER_X16);
    if (result != NRF_BME280_OK) {
        return result;
    }
    result = bme280_write_reg(
        device,
        BME280_REG_CTRL_MEAS,
        BME280_CTRL_MEAS_FORCED);
    if (result != NRF_BME280_OK) {
        return result;
    }

    /* Forced mode must be latched now and return to sleep after this conversion. */
    uint8_t ctrl = 0U;
    result = bme280_read_regs(device, BME280_REG_CTRL_MEAS, &ctrl, 1U);
    if (result != NRF_BME280_OK || ctrl != BME280_CTRL_MEAS_FORCED) {
        device->initialized = false;
        device->state = NRF_BME280_RECOVER;
        return result != NRF_BME280_OK ? result : NRF_BME280_ERR_CONFIG;
    }

    device->conversion_ready_ms = now_ms + BME280_MEASUREMENT_DELAY_MS;
    device->conversion_deadline_ms = device->conversion_ready_ms +
        (BME280_MEASUREMENT_POLL_ATTEMPTS - 1U) * BME280_MEASUREMENT_POLL_DELAY_MS;
    device->conversion_pending = true;
    return NRF_BME280_OK;
}

nrf_bme280_result_t nrf_bme280_poll_conversion(
    nrf_bme280_t *device, uint64_t now_ms, nrf_env_sample_t *sample)
{
    if (sample == NULL || device == NULL) return NRF_BME280_ERR_ARGUMENT;
    *sample = (nrf_env_sample_t){0};
    if (!device->initialized || device->state != NRF_BME280_MEASURING || !device->conversion_pending)
        return NRF_BME280_ERR_NOT_INITIALIZED;
    if (now_ms < device->conversion_ready_ms) return NRF_BME280_PENDING;
    if (now_ms > device->conversion_deadline_ms) {
        device->conversion_pending = false;
        device->initialized = false;
        device->state = NRF_BME280_RECOVER;
        return NRF_BME280_ERR_MEASUREMENT;
    }
    uint8_t config[3] = {0};
    nrf_bme280_result_t result = bme280_read_regs(device, BME280_REG_STATUS, config, sizeof(config));
    if (result != NRF_BME280_OK) {
        device->conversion_pending = false;
        return result;
    }
    if ((config[0] & (BME280_STATUS_MEASURING | BME280_STATUS_IM_UPDATE)) != 0U) {
        if (now_ms < device->conversion_deadline_ms) return NRF_BME280_PENDING;
        device->conversion_pending = false;
        return NRF_BME280_ERR_MEASUREMENT;
    }
    device->conversion_pending = false;
    if (config[1] != BME280_CTRL_MEAS_SLEEP ||
        (config[2] & 0x1CU) != BME280_CONFIG_FILTER_X16) {
        device->initialized = false;
        device->state = NRF_BME280_RECOVER;
        return NRF_BME280_ERR_CONFIG;
    }
    if (device->has_humidity) {
        uint8_t humidity_config = 0U;
        result = bme280_read_regs(device, BME280_REG_CTRL_HUM, &humidity_config, 1U);
        if (result != NRF_BME280_OK) return result;
        if ((humidity_config & 0x07U) != BME280_CTRL_HUM_X1) {
            device->initialized = false;
            device->state = NRF_BME280_RECOVER;
            return NRF_BME280_ERR_CONFIG;
        }
    }
    uint8_t data[8] = {0};
    result = bme280_read_regs(
        device, BME280_REG_DATA, data, device->has_humidity ? sizeof(data) : BMP280_DATA_BYTES);
    if (result != NRF_BME280_OK) {
        return result;
    }

    const uint32_t raw_pressure =
        ((uint32_t)data[0] << 12U) |
        ((uint32_t)data[1] << 4U) |
        ((uint32_t)data[2] >> 4U);
    const uint32_t raw_temperature =
        ((uint32_t)data[3] << 12U) |
        ((uint32_t)data[4] << 4U) |
        ((uint32_t)data[5] >> 4U);
    const uint32_t raw_humidity =
        ((uint32_t)data[6] << 8U) | (uint32_t)data[7];

    if (raw_pressure == 0x80000U || raw_temperature == 0x80000U ||
        (device->has_humidity && raw_humidity == 0x8000U)) {
        return NRF_BME280_ERR_MEASUREMENT;
    }

    int32_t t_fine = 0;
    const int32_t temperature_centi_c = bme280_compensate_temperature_centi_c(
        raw_temperature,
        &device->calibration,
        &t_fine);
    const uint32_t pressure_x100_pa = bme280_compensate_pressure_x100_pa(
        raw_pressure,
        t_fine,
        &device->calibration);
    const uint32_t humidity_x1024_percent = device->has_humidity
        ? bme280_compensate_humidity_x1024_percent(
              raw_humidity,
              t_fine,
              &device->calibration)
        : 0U; /* unused: a BMP280 reports NRF_ENV_HUMIDITY_UNAVAILABLE below */

    sample->temperature_milli_c = temperature_centi_c * 10;
    sample->pressure_pa = (pressure_x100_pa + 50U) / 100U;
    sample->humidity_milli_percent = device->has_humidity
        ? (humidity_x1024_percent * 1000U + 512U) / 1024U
        : NRF_ENV_HUMIDITY_UNAVAILABLE;
    return NRF_BME280_OK;
}

int nrf_bme280_read_env(void *context, nrf_env_sample_t *sample)
{
    nrf_bme280_t *device = (nrf_bme280_t *)context;
    if (sample == NULL || device == NULL) return NRF_BME280_ERR_ARGUMENT;
    *sample = (nrf_env_sample_t){0};
    nrf_bme280_result_t result = nrf_bme280_start_conversion(device, 0U);
    if (result != NRF_BME280_OK) return result;
    device->delay_ms(device->delay_context, BME280_MEASUREMENT_DELAY_MS);
    for (uint32_t attempt = 0U; attempt < BME280_MEASUREMENT_POLL_ATTEMPTS; ++attempt) {
        result = nrf_bme280_poll_conversion(device, BME280_MEASUREMENT_DELAY_MS +
            attempt * BME280_MEASUREMENT_POLL_DELAY_MS, sample);
        if (result != NRF_BME280_PENDING) return result;
        device->delay_ms(device->delay_context, BME280_MEASUREMENT_POLL_DELAY_MS);
    }
    return NRF_BME280_ERR_MEASUREMENT;
}
