#include "nrf_bmi160.h"

#include <string.h>

/* Bosch BMI160 register map / commands used by this minimal runtime driver. */
enum {
    BMI160_REG_CHIP_ID = 0x00U,
    BMI160_REG_PMU_STATUS = 0x03U,
    BMI160_REG_GYRO_DATA = 0x0CU,
    BMI160_REG_ACCEL_CONFIG = 0x40U,
    BMI160_REG_ACCEL_RANGE = 0x41U,
    BMI160_REG_GYRO_CONFIG = 0x42U,
    BMI160_REG_GYRO_RANGE = 0x43U,
    BMI160_REG_CMD = 0x7EU,
    BMI160_REG_TEMPERATURE = 0x20U,

    BMI160_CMD_SOFT_RESET = 0xB6U,
    BMI160_CMD_ACCEL_NORMAL = 0x11U,
    BMI160_CMD_GYRO_NORMAL = 0x15U,

    BMI160_ACCEL_CONFIG_100HZ_NORMAL = 0x28U,
    BMI160_ACCEL_RANGE_2G = 0x03U,
    BMI160_GYRO_CONFIG_100HZ_NORMAL = 0x28U,
    BMI160_GYRO_RANGE_250DPS = 0x03U,

    BMI160_PMU_ACCEL_MASK = 0x30U,
    BMI160_PMU_GYRO_MASK = 0x0CU,
    BMI160_PMU_ACCEL_NORMAL = 0x10U,
    BMI160_PMU_GYRO_NORMAL = 0x04U,

    BMI160_SOFT_RESET_DELAY_MS = 1U,
    BMI160_ACCEL_NORMAL_DELAY_MS = 5U,
    BMI160_GYRO_NORMAL_DELAY_MS = 80U,
    BMI160_WRITE_SETTLE_DELAY_MS = 1U,
};

static nrf_bmi160_result_t bmi160_write_reg(
    nrf_bmi160_t *device,
    uint8_t reg,
    uint8_t value)
{
    const uint8_t packet[2] = {reg, value};
    return nrf_sensor_bus_write(
               &device->bus,
               device->address,
               packet,
               sizeof(packet)) == NRF_SENSOR_BUS_OK
               ? NRF_BMI160_OK
               : NRF_BMI160_ERR_BUS;
}

static nrf_bmi160_result_t bmi160_read_regs(
    const nrf_bmi160_t *device,
    uint8_t reg,
    uint8_t *data,
    size_t data_len)
{
    return nrf_sensor_bus_write_read(
               &device->bus,
               device->address,
               &reg,
               1U,
               data,
               data_len) == NRF_SENSOR_BUS_OK
               ? NRF_BMI160_OK
               : NRF_BMI160_ERR_BUS;
}

static int16_t bmi160_read_le_i16(const uint8_t *data)
{
    const uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
    return (int16_t)value;
}

static int32_t bmi160_round_scaled(
    int16_t raw,
    int32_t full_scale_units)
{
    const int64_t numerator = (int64_t)raw * (int64_t)full_scale_units;
    const int64_t half = INT64_C(16384);
    if (numerator >= 0) {
        return (int32_t)((numerator + half) / INT64_C(32768));
    }
    return (int32_t)((numerator - half) / INT64_C(32768));
}

static int32_t bmi160_temperature_milli_c(int16_t raw)
{
    const int64_t numerator = (int64_t)raw * INT64_C(1000);
    const int64_t rounded = numerator >= 0
                                ? numerator + INT64_C(256)
                                : numerator - INT64_C(256);
    return 23000 + (int32_t)(rounded / INT64_C(512));
}

nrf_bmi160_result_t nrf_bmi160_init(
    nrf_bmi160_t *device,
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    nrf_bmi160_delay_ms_fn delay_ms,
    void *delay_context)
{
    uint8_t value = 0U;
    uint8_t config[4] = {0};
    nrf_bmi160_result_t result;

    if (device == NULL || bus == NULL || bus->transfer == NULL ||
        delay_ms == NULL || !nrf_sensor_bus_is_valid_address(address)) {
        return NRF_BMI160_ERR_ARGUMENT;
    }

    *device = (nrf_bmi160_t){
        .bus = *bus,
        .delay_ms = delay_ms,
        .delay_context = delay_context,
        .address = address,
        .initialized = false,
    };

    /*
     * Read the id first: the soft reset below is a write, and a write must not go to a device that is not a
     * BMI160 (another part answering on 0x68/0x69 would take 0xB6 as a register value).
     */
    result = bmi160_read_regs(device, BMI160_REG_CHIP_ID, &value, 1U);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->chip_id = value;
    if (value != NRF_BMI160_CHIP_ID) {
        return NRF_BMI160_ERR_CHIP_ID;
    }

    result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_SOFT_RESET);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_SOFT_RESET_DELAY_MS);

    result = bmi160_read_regs(device, BMI160_REG_CHIP_ID, &value, 1U);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->chip_id = value;
    if (value != NRF_BMI160_CHIP_ID) {
        return NRF_BMI160_ERR_CHIP_ID;
    }

    result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_ACCEL_NORMAL);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_ACCEL_NORMAL_DELAY_MS);

    result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_GYRO_NORMAL);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_GYRO_NORMAL_DELAY_MS);

    result = bmi160_read_regs(device, BMI160_REG_PMU_STATUS, &value, 1U);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    if ((value & (BMI160_PMU_ACCEL_MASK | BMI160_PMU_GYRO_MASK)) !=
        (BMI160_PMU_ACCEL_NORMAL | BMI160_PMU_GYRO_NORMAL)) {
        return NRF_BMI160_ERR_PMU;
    }

    result = bmi160_write_reg(
        device,
        BMI160_REG_ACCEL_CONFIG,
        BMI160_ACCEL_CONFIG_100HZ_NORMAL);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_WRITE_SETTLE_DELAY_MS);

    result = bmi160_write_reg(
        device,
        BMI160_REG_ACCEL_RANGE,
        BMI160_ACCEL_RANGE_2G);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_WRITE_SETTLE_DELAY_MS);

    result = bmi160_write_reg(
        device,
        BMI160_REG_GYRO_CONFIG,
        BMI160_GYRO_CONFIG_100HZ_NORMAL);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_WRITE_SETTLE_DELAY_MS);

    result = bmi160_write_reg(
        device,
        BMI160_REG_GYRO_RANGE,
        BMI160_GYRO_RANGE_250DPS);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    device->delay_ms(device->delay_context, BMI160_WRITE_SETTLE_DELAY_MS);

    result = bmi160_read_regs(
        device,
        BMI160_REG_ACCEL_CONFIG,
        config,
        sizeof(config));
    if (result != NRF_BMI160_OK) {
        return result;
    }
    if (config[0] != BMI160_ACCEL_CONFIG_100HZ_NORMAL ||
        config[1] != BMI160_ACCEL_RANGE_2G ||
        config[2] != BMI160_GYRO_CONFIG_100HZ_NORMAL ||
        config[3] != BMI160_GYRO_RANGE_250DPS) {
        return NRF_BMI160_ERR_CONFIG;
    }

    device->initialized = true;
    return NRF_BMI160_OK;
}

int nrf_bmi160_read_imu(void *context, nrf_imu_sample_t *sample)
{
    nrf_bmi160_t *device = (nrf_bmi160_t *)context;
    uint8_t motion_data[12] = {0};
    uint8_t temperature_data[2] = {0};
    nrf_bmi160_result_t result;

    if (sample == NULL || device == NULL) {
        return NRF_BMI160_ERR_ARGUMENT;
    }

    *sample = (nrf_imu_sample_t){0};
    if (!device->initialized) {
        return NRF_BMI160_ERR_NOT_INITIALIZED;
    }

    /*
     * A power loss or reset leaves the sensor ACKing in suspend mode with all
     * data registers at zero, which would read as a valid motionless sample.
     */
    uint8_t pmu_status = 0U;
    result = bmi160_read_regs(device, BMI160_REG_PMU_STATUS, &pmu_status, 1U);
    if (result != NRF_BMI160_OK) {
        return result;
    }
    if ((pmu_status & (BMI160_PMU_ACCEL_MASK | BMI160_PMU_GYRO_MASK)) !=
        (BMI160_PMU_ACCEL_NORMAL | BMI160_PMU_GYRO_NORMAL)) {
        device->initialized = false;
        return NRF_BMI160_ERR_PMU;
    }

    result = bmi160_read_regs(
        device,
        BMI160_REG_GYRO_DATA,
        motion_data,
        sizeof(motion_data));
    if (result != NRF_BMI160_OK) {
        return result;
    }

    result = bmi160_read_regs(
        device,
        BMI160_REG_TEMPERATURE,
        temperature_data,
        sizeof(temperature_data));
    if (result != NRF_BMI160_OK) {
        return result;
    }

    const int16_t temperature_raw = bmi160_read_le_i16(temperature_data);
    if (temperature_raw == INT16_MIN) return NRF_BMI160_ERR_MEASUREMENT;

    for (size_t axis = 0U; axis < 3U; ++axis) {
        const int16_t gyro_raw = bmi160_read_le_i16(&motion_data[axis * 2U]);
        const int16_t accel_raw =
            bmi160_read_le_i16(&motion_data[6U + axis * 2U]);

        sample->gyro_mdps[axis] =
            bmi160_round_scaled(gyro_raw, 250000);
        sample->accel_mg[axis] =
            bmi160_round_scaled(accel_raw, 2000);
    }

    sample->temperature_milli_c = bmi160_temperature_milli_c(temperature_raw);

    return NRF_BMI160_OK;
}
