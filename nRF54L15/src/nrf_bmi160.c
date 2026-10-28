#include "nrf_bmi160.h"

#include <string.h>

/* Bosch BMI160 register map / commands used by this minimal runtime driver. */
enum {
    BMI160_REG_CHIP_ID = 0x00U,
    BMI160_REG_ERROR = 0x02U,
    BMI160_REG_PMU_STATUS = 0x03U,
    BMI160_REG_GYRO_DATA = 0x0CU,
    BMI160_REG_STATUS = 0x1BU,
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
    BMI160_PROBE_ATTEMPTS = 3U,
    BMI160_PROBE_RETRY_DELAY_MS = 5U,
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

static void bmi160_capture_error(nrf_bmi160_t *device)
{
    /* ERR_REG clears flags on read; keep it only when entering recovery. */
    device->err_reg = 0U;
    device->err_reg_valid = device->chip_id == NRF_BMI160_CHIP_ID &&
        bmi160_read_regs(device, BMI160_REG_ERROR, &device->err_reg, 1U) == NRF_BMI160_OK;
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

nrf_bmi160_result_t nrf_bmi160_init_observed(
    nrf_bmi160_t *device,
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    nrf_bmi160_delay_ms_fn delay_ms,
    void *delay_context,
    nrf_bmi160_state_observer_fn observer,
    void *observer_context)
{
    if (delay_ms == NULL) return NRF_BMI160_ERR_ARGUMENT;
    nrf_bmi160_result_t result = nrf_bmi160_begin_async(device, bus, address, observer, observer_context);
    if (result == NRF_BMI160_PENDING) {
        device->delay_ms = delay_ms;
        device->delay_context = delay_context;
    }
    uint64_t now_ms = 0U;
    while (result == NRF_BMI160_PENDING) {
        result = nrf_bmi160_step_async(device, now_ms);
        if (result == NRF_BMI160_PENDING && device->prepare_due_ms > now_ms) {
            const uint32_t delay = (uint32_t)(device->prepare_due_ms - now_ms);
            delay_ms(delay_context, delay);
            now_ms = device->prepare_due_ms;
        }
    }
    return result;
}

nrf_bmi160_result_t nrf_bmi160_init(
    nrf_bmi160_t *device, const nrf_sensor_bus_t *bus, uint8_t address,
    nrf_bmi160_delay_ms_fn delay_ms, void *delay_context)
{
    return nrf_bmi160_init_observed(device, bus, address, delay_ms, delay_context, NULL, NULL);
}

nrf_bmi160_result_t nrf_bmi160_begin_async(
    nrf_bmi160_t *device, const nrf_sensor_bus_t *bus, uint8_t address,
    nrf_bmi160_state_observer_fn observer, void *observer_context)
{
    if (device == NULL || bus == NULL || bus->transfer == NULL ||
        !nrf_sensor_bus_is_valid_address(address)) return NRF_BMI160_ERR_ARGUMENT;
    *device = (nrf_bmi160_t){.bus = *bus, .address = address, .state = NRF_BMI160_PROBE,
                              .observer = observer, .observer_context = observer_context};
    if (observer != NULL) observer(observer_context, device->state);
    return NRF_BMI160_PENDING;
}

nrf_bmi160_result_t nrf_bmi160_step_async(nrf_bmi160_t *device, uint64_t now_ms)
{
    uint8_t value = 0U;
    nrf_bmi160_result_t result = NRF_BMI160_OK;
    uint32_t delay = 0U;
    if (device == NULL) return NRF_BMI160_ERR_ARGUMENT;
    if (device->state == NRF_BMI160_MEASURING) return NRF_BMI160_OK;
    if (device->state == NRF_BMI160_RECOVER) return NRF_BMI160_ERR_NOT_INITIALIZED;
    if (now_ms < device->prepare_due_ms) return NRF_BMI160_PENDING;
    if (device->state == NRF_BMI160_PROBE) {
        result = bmi160_read_regs(device, BMI160_REG_CHIP_ID, &value, 1U);
        /* Match Bosch SensorAPI: a transient ID read must not reject this address. */
        if ((result != NRF_BMI160_OK || value != NRF_BMI160_CHIP_ID) &&
            ++device->prepare_phase < BMI160_PROBE_ATTEMPTS) {
            /* Give a recently powered sensor time to start accepting I2C.
             * Without a deadline all attempts execute in the same tick. */
            device->prepare_due_ms = now_ms > UINT64_MAX - BMI160_PROBE_RETRY_DELAY_MS
                ? UINT64_MAX : now_ms + BMI160_PROBE_RETRY_DELAY_MS;
            return NRF_BMI160_PENDING;
        }
        if (result != NRF_BMI160_OK) goto failed_async;
        device->chip_id = value;
        if (value != NRF_BMI160_CHIP_ID) { result = NRF_BMI160_ERR_CHIP_ID; goto failed_async; }
        device->prepare_phase = 0U;
        device->state = NRF_BMI160_CONFIGURE;
        if (device->observer != NULL) device->observer(device->observer_context, device->state);
        return NRF_BMI160_PENDING;
    }
    if (device->state == NRF_BMI160_VERIFY) {
        uint8_t config[4] = {0};
        result = bmi160_read_regs(device, BMI160_REG_ACCEL_CONFIG, config, sizeof(config));
        if (result != NRF_BMI160_OK) goto failed_async;
        if (config[0] != BMI160_ACCEL_CONFIG_100HZ_NORMAL || config[1] != BMI160_ACCEL_RANGE_2G ||
            config[2] != BMI160_GYRO_CONFIG_100HZ_NORMAL || config[3] != BMI160_GYRO_RANGE_250DPS) {
            result = NRF_BMI160_ERR_CONFIG; goto failed_async;
        }
        device->initialized = true;
        device->state = NRF_BMI160_MEASURING;
        if (device->observer != NULL) device->observer(device->observer_context, device->state);
        return NRF_BMI160_OK;
    }
    if (device->state != NRF_BMI160_CONFIGURE) return NRF_BMI160_ERR_NOT_INITIALIZED;
    switch (device->prepare_phase) {
    case 0:
        result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_SOFT_RESET);
        delay = BMI160_SOFT_RESET_DELAY_MS;
        break;
    case 1:
        result = bmi160_read_regs(device, BMI160_REG_CHIP_ID, &value, 1U);
        if (result != NRF_BMI160_OK) break;
        device->chip_id = value;
        if (value != NRF_BMI160_CHIP_ID) { result = NRF_BMI160_ERR_CHIP_ID; break; }
        result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_ACCEL_NORMAL);
        delay = BMI160_ACCEL_NORMAL_DELAY_MS;
        break;
    case 2:
        result = bmi160_write_reg(device, BMI160_REG_CMD, BMI160_CMD_GYRO_NORMAL);
        delay = BMI160_GYRO_NORMAL_DELAY_MS;
        break;
    case 3:
        result = bmi160_read_regs(device, BMI160_REG_PMU_STATUS, &value, 1U);
        if (result != NRF_BMI160_OK) break;
        if ((value & (BMI160_PMU_ACCEL_MASK | BMI160_PMU_GYRO_MASK)) !=
            (BMI160_PMU_ACCEL_NORMAL | BMI160_PMU_GYRO_NORMAL)) { result = NRF_BMI160_ERR_PMU; break; }
        result = bmi160_write_reg(device, BMI160_REG_ACCEL_CONFIG, BMI160_ACCEL_CONFIG_100HZ_NORMAL);
        delay = BMI160_WRITE_SETTLE_DELAY_MS;
        break;
    case 4:
        result = bmi160_write_reg(device, BMI160_REG_ACCEL_RANGE, BMI160_ACCEL_RANGE_2G);
        delay = BMI160_WRITE_SETTLE_DELAY_MS;
        break;
    case 5:
        result = bmi160_write_reg(device, BMI160_REG_GYRO_CONFIG, BMI160_GYRO_CONFIG_100HZ_NORMAL);
        delay = BMI160_WRITE_SETTLE_DELAY_MS;
        break;
    case 6:
        result = bmi160_write_reg(device, BMI160_REG_GYRO_RANGE, BMI160_GYRO_RANGE_250DPS);
        delay = BMI160_WRITE_SETTLE_DELAY_MS;
        break;
    default:
        device->state = NRF_BMI160_VERIFY;
        if (device->observer != NULL) device->observer(device->observer_context, device->state);
        return NRF_BMI160_PENDING;
    }
    if (result != NRF_BMI160_OK) goto failed_async;
    ++device->prepare_phase;
    device->prepare_due_ms = now_ms > UINT64_MAX - delay ? UINT64_MAX : now_ms + delay;
    return NRF_BMI160_PENDING;

failed_async:
    bmi160_capture_error(device);
    device->initialized = false;
    device->state = NRF_BMI160_RECOVER;
    if (device->observer != NULL) device->observer(device->observer_context, device->state);
    return result;
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
    if (!device->initialized || device->state != NRF_BMI160_MEASURING) {
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
        bmi160_capture_error(device);
        device->initialized = false;
        device->state = NRF_BMI160_RECOVER;
        return NRF_BMI160_ERR_PMU;
    }

    uint8_t status = 0U;
    result = bmi160_read_regs(device, BMI160_REG_STATUS, &status, 1U);
    if (result != NRF_BMI160_OK) return result;
    if ((status & 0xC0U) != 0xC0U) return NRF_BMI160_PENDING;

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
