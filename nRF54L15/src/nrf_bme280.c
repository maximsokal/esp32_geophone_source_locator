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

    for (size_t index = 0U; index < 2U; ++index) {
        uint8_t chip_id = 0U;
        const nrf_bme280_result_t result = bme280_read_regs_at(
            &device->bus,
            addresses[index],
            BME280_REG_CHIP_ID,
            &chip_id,
            1U);
        if (result != NRF_BME280_OK) {
            continue;
        }

        saw_response = true;
        if (chip_id == NRF_BME280_CHIP_ID) {
            device->address = addresses[index];
            return NRF_BME280_OK;
        }
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

static nrf_bme280_result_t bme280_wait_for_measurement(nrf_bme280_t *device)
{
    for (uint32_t attempt = 0U;
         attempt < BME280_MEASUREMENT_POLL_ATTEMPTS;
         ++attempt) {
        uint8_t status = 0U;
        const nrf_bme280_result_t result = bme280_read_regs(
            device,
            BME280_REG_STATUS,
            &status,
            1U);
        if (result != NRF_BME280_OK) {
            return result;
        }
        if ((status & BME280_STATUS_MEASURING) == 0U) {
            return NRF_BME280_OK;
        }
        if (attempt + 1U < BME280_MEASUREMENT_POLL_ATTEMPTS) {
            device->delay_ms(
                device->delay_context,
                BME280_MEASUREMENT_POLL_DELAY_MS);
        }
    }
    return NRF_BME280_ERR_MEASUREMENT;
}

nrf_bme280_result_t nrf_bme280_init(
    nrf_bme280_t *device,
    const nrf_sensor_bus_t *bus,
    nrf_bme280_delay_ms_fn delay_ms,
    void *delay_context)
{
    if (device == NULL || bus == NULL || bus->transfer == NULL || delay_ms == NULL) {
        return NRF_BME280_ERR_ARGUMENT;
    }

    *device = (nrf_bme280_t){
        .bus = *bus,
        .delay_ms = delay_ms,
        .delay_context = delay_context,
        .address = 0U,
        .initialized = false,
    };

    nrf_bme280_result_t result = bme280_probe(device);
    if (result != NRF_BME280_OK) {
        return result;
    }

    result = bme280_write_reg(device, BME280_REG_RESET, BME280_SOFT_RESET);
    if (result != NRF_BME280_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BME280_STARTUP_DELAY_MS);

    result = bme280_wait_for_nvm(device);
    if (result != NRF_BME280_OK) {
        return result;
    }

    uint8_t temp_press_calib[26] = {0};
    uint8_t humidity_calib[7] = {0};
    result = bme280_read_regs(
        device,
        BME280_REG_TEMP_PRESS_CALIB,
        temp_press_calib,
        sizeof(temp_press_calib));
    if (result != NRF_BME280_OK) {
        return result;
    }
    result = bme280_read_regs(
        device,
        BME280_REG_HUMIDITY_CALIB,
        humidity_calib,
        sizeof(humidity_calib));
    if (result != NRF_BME280_OK) {
        return result;
    }

    bme280_parse_calibration(
        &device->calibration,
        temp_press_calib,
        humidity_calib);
    if (device->calibration.dig_p1 == 0U) {
        return NRF_BME280_ERR_CALIBRATION;
    }

    result = bme280_write_reg(device, BME280_REG_CTRL_HUM, BME280_CTRL_HUM_X1);
    if (result != NRF_BME280_OK) {
        return result;
    }
    result = bme280_write_reg(device, BME280_REG_CONFIG, BME280_CONFIG_FILTER_X16);
    if (result != NRF_BME280_OK) {
        return result;
    }
    /* Writing CTRL_MEAS latches the humidity oversampling setting. */
    result = bme280_write_reg(device, BME280_REG_CTRL_MEAS, BME280_CTRL_MEAS_SLEEP);
    if (result != NRF_BME280_OK) {
        return result;
    }

    uint8_t config[4] = {0};
    result = bme280_read_regs(
        device,
        BME280_REG_CTRL_HUM,
        config,
        sizeof(config));
    if (result != NRF_BME280_OK) {
        return result;
    }
    if ((config[0] & 0x07U) != BME280_CTRL_HUM_X1 ||
        (config[2] & 0xFCU) != BME280_CTRL_MEAS_SLEEP ||
        (config[2] & 0x03U) != 0U ||
        (config[3] & 0x1CU) != BME280_CONFIG_FILTER_X16) {
        return NRF_BME280_ERR_CONFIG;
    }

    device->initialized = true;
    return NRF_BME280_OK;
}

int nrf_bme280_read_env(void *context, nrf_env_sample_t *sample)
{
    nrf_bme280_t *device = (nrf_bme280_t *)context;
    if (sample == NULL || device == NULL) {
        return NRF_BME280_ERR_ARGUMENT;
    }

    *sample = (nrf_env_sample_t){0};
    if (!device->initialized) {
        return NRF_BME280_ERR_NOT_INITIALIZED;
    }

    nrf_bme280_result_t result = bme280_write_reg(
        device,
        BME280_REG_CTRL_MEAS,
        BME280_CTRL_MEAS_FORCED);
    if (result != NRF_BME280_OK) {
        return result;
    }

    device->delay_ms(device->delay_context, BME280_MEASUREMENT_DELAY_MS);
    result = bme280_wait_for_measurement(device);
    if (result != NRF_BME280_OK) {
        return result;
    }

    uint8_t data[8] = {0};
    result = bme280_read_regs(device, BME280_REG_DATA, data, sizeof(data));
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
        raw_humidity == 0x8000U) {
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
    const uint32_t humidity_x1024_percent =
        bme280_compensate_humidity_x1024_percent(
            raw_humidity,
            t_fine,
            &device->calibration);

    sample->temperature_milli_c = temperature_centi_c * 10;
    sample->pressure_pa = (pressure_x100_pa + 50U) / 100U;
    sample->humidity_milli_percent =
        (humidity_x1024_percent * 1000U + 512U) / 1024U;
    return NRF_BME280_OK;
}
