#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_bmi160.h"

#define TEST_BMI160_REG_CHIP_ID 0x00U
#define TEST_BMI160_REG_PMU_STATUS 0x03U
#define TEST_BMI160_REG_GYRO_DATA 0x0CU
#define TEST_BMI160_REG_TEMPERATURE 0x20U
#define TEST_BMI160_REG_ACCEL_CONFIG 0x40U
#define TEST_BMI160_REG_ACCEL_RANGE 0x41U
#define TEST_BMI160_REG_GYRO_CONFIG 0x42U
#define TEST_BMI160_REG_GYRO_RANGE 0x43U
#define TEST_BMI160_REG_CMD 0x7EU

#define TEST_BMI160_CMD_ACCEL_NORMAL 0x11U
#define TEST_BMI160_CMD_GYRO_NORMAL 0x15U

#define TEST_BMI160_PMU_ACCEL_NORMAL 0x10U
#define TEST_BMI160_PMU_GYRO_NORMAL 0x04U

typedef struct {
    uint8_t regs[256];
    int transfer_count;
    int fail_at_transfer;
    bool emulate_pmu;
    uint8_t last_address;
    nrf_bmi160_t *observed_device;
    nrf_bmi160_state_t observed_states[32];
    nrf_bmi160_state_t transitions[5];
    size_t transition_count;
    uint32_t delays[16];
    size_t delay_count;
} fake_bmi160_bus_t;

static void observe_state(void *context, nrf_bmi160_state_t state)
{
    fake_bmi160_bus_t *fake = context;
    assert(fake->transition_count < 5U);
    fake->transitions[fake->transition_count++] = state;
}

static int fake_transfer(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    fake_bmi160_bus_t *fake = (fake_bmi160_bus_t *)context;
    assert(fake != NULL);

    fake->transfer_count++;
    if (fake->observed_device != NULL) {
        assert(fake->transfer_count <= (int)(sizeof(fake->observed_states) / sizeof(fake->observed_states[0])));
        fake->observed_states[fake->transfer_count - 1] = fake->observed_device->state;
    }
    fake->last_address = address;
    if (fake->fail_at_transfer > 0 &&
        fake->transfer_count == fake->fail_at_transfer) {
        return -1;
    }

    if (write_len == 2U && read_len == 0U && write_data != NULL) {
        const uint8_t reg = write_data[0];
        const uint8_t value = write_data[1];
        fake->regs[reg] = value;

        if (fake->emulate_pmu && reg == TEST_BMI160_REG_CMD) {
            if (value == TEST_BMI160_CMD_ACCEL_NORMAL) {
                fake->regs[TEST_BMI160_REG_PMU_STATUS] =
                    (uint8_t)((fake->regs[TEST_BMI160_REG_PMU_STATUS] & 0xCFU) |
                              TEST_BMI160_PMU_ACCEL_NORMAL);
            } else if (value == TEST_BMI160_CMD_GYRO_NORMAL) {
                fake->regs[TEST_BMI160_REG_PMU_STATUS] =
                    (uint8_t)((fake->regs[TEST_BMI160_REG_PMU_STATUS] & 0xF3U) |
                              TEST_BMI160_PMU_GYRO_NORMAL);
            }
        }
        return 0;
    }

    if (write_len == 1U && read_len > 0U &&
        write_data != NULL && read_data != NULL) {
        const uint8_t reg = write_data[0];
        memcpy(read_data, &fake->regs[reg], read_len);
        return 0;
    }

    return -2;
}

static void fake_delay_ms(void *context, uint32_t delay_ms)
{
    fake_bmi160_bus_t *fake = (fake_bmi160_bus_t *)context;
    assert(fake != NULL);
    assert(fake->delay_count < (sizeof(fake->delays) / sizeof(fake->delays[0])));
    fake->delays[fake->delay_count++] = delay_ms;
}

static nrf_sensor_bus_t make_bus(fake_bmi160_bus_t *fake)
{
    nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = fake,
    };
    return bus;
}

static void seed_sensor(fake_bmi160_bus_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->regs[TEST_BMI160_REG_CHIP_ID] = NRF_BMI160_CHIP_ID;
    fake->emulate_pmu = true;
}

static void put_i16_le(uint8_t *target, int16_t value)
{
    const uint16_t encoded = (uint16_t)value;
    target[0] = (uint8_t)(encoded & 0xFFU);
    target[1] = (uint8_t)(encoded >> 8U);
}

static void assert_sample_zero(const nrf_imu_sample_t *sample)
{
    assert(sample != NULL);
    for (size_t axis = 0U; axis < 3U; ++axis) {
        assert(sample->accel_mg[axis] == 0);
        assert(sample->gyro_mdps[axis] == 0);
    }
    assert(sample->temperature_milli_c == 0);
}

static void test_init_rejects_invalid_arguments(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;

    assert(nrf_bmi160_init(
               NULL,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_ARGUMENT);
    assert(nrf_bmi160_init(
               &device,
               NULL,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_ARGUMENT);
    assert(nrf_bmi160_init(
               &device,
               &bus,
               0U,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_ARGUMENT);
    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               NULL,
               &fake) == NRF_BMI160_ERR_ARGUMENT);
}

static void test_init_configures_expected_runtime_profile(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_OK);

    assert(device.initialized);
    assert(device.address == NRF_BMI160_DEFAULT_ADDRESS);
    assert(fake.last_address == NRF_BMI160_DEFAULT_ADDRESS);
    assert(fake.transfer_count == 11); /* id read, soft reset, id read, ... */

    assert(fake.regs[TEST_BMI160_REG_PMU_STATUS] ==
           (TEST_BMI160_PMU_ACCEL_NORMAL | TEST_BMI160_PMU_GYRO_NORMAL));
    assert(fake.regs[TEST_BMI160_REG_ACCEL_CONFIG] == 0x28U);
    assert(fake.regs[TEST_BMI160_REG_ACCEL_RANGE] == 0x03U);
    assert(fake.regs[TEST_BMI160_REG_GYRO_CONFIG] == 0x28U);
    assert(fake.regs[TEST_BMI160_REG_GYRO_RANGE] == 0x03U);

    assert(fake.delay_count == 7U);
    assert(fake.delays[0] == 1U);
    assert(fake.delays[1] == 5U);
    assert(fake.delays[2] == 80U);
    assert(fake.delays[3] == 1U);
    assert(fake.delays[4] == 1U);
    assert(fake.delays[5] == 1U);
    assert(fake.delays[6] == 1U);
}

static void test_init_rejects_wrong_chip_id_before_powerup(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    fake.regs[TEST_BMI160_REG_CHIP_ID] = 0x00U;
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_CHIP_ID);
    assert(!device.initialized);
    assert(device.chip_id == 0x00U);
    /* Only the id was read: nothing is written to a device that is not a BMI160, not even the soft reset. */
    assert(fake.transfer_count == 1);
    assert(fake.delay_count == 0U);
}

static void test_init_rejects_pmu_that_did_not_enter_normal_mode(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    fake.emulate_pmu = false;
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_PMU);
    assert(!device.initialized);
    assert(fake.transfer_count == 6);
}

static void test_init_propagates_transfer_failure(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    fake.fail_at_transfer = 1;
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_ERR_BUS);
    assert(!device.initialized);
    assert(fake.transfer_count == 1);
    assert(fake.delay_count == 0U);
}

static void test_read_requires_initialized_device_and_clears_output(void)
{
    nrf_bmi160_t device = {0};
    nrf_imu_sample_t sample;
    memset(&sample, 0x7F, sizeof(sample));

    assert(nrf_bmi160_read_imu(&device, &sample) ==
           NRF_BMI160_ERR_NOT_INITIALIZED);
    assert_sample_zero(&sample);
    assert(nrf_bmi160_read_imu(NULL, &sample) == NRF_BMI160_ERR_ARGUMENT);
    assert(nrf_bmi160_read_imu(&device, NULL) == NRF_BMI160_ERR_ARGUMENT);
}

static void test_read_converts_motion_and_temperature_without_float(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;
    nrf_imu_sample_t sample;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_OK);

    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 0U], 16384);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 2U], -16384);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 4U], 1);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 6U], 16384);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 8U], -8192);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 10U], 10);
    put_i16_le(&fake.regs[TEST_BMI160_REG_TEMPERATURE], -256);

    assert(nrf_bmi160_read_imu(&device, &sample) == NRF_BMI160_OK);

    assert(sample.gyro_mdps[0] == 125000);
    assert(sample.gyro_mdps[1] == -125000);
    assert(sample.gyro_mdps[2] == 8);
    assert(sample.accel_mg[0] == 1000);
    assert(sample.accel_mg[1] == -500);
    assert(sample.accel_mg[2] == 1);
    assert(sample.temperature_milli_c == 22500);
}

static void test_read_transfer_failure_does_not_leak_stale_sample(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;
    nrf_imu_sample_t sample;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_OK);

    fake.fail_at_transfer = fake.transfer_count + 1;
    memset(&sample, 0x55, sizeof(sample));
    assert(nrf_bmi160_read_imu(&device, &sample) == NRF_BMI160_ERR_BUS);
    assert_sample_zero(&sample);
}

/*
 * After a power loss or reset the BMI160 ACKs again but is in suspend mode and
 * its data registers read zero. That must not look like a valid still-sensor
 * sample (0 g, 0 dps), and the driver must report that it needs re-init.
 */
static void test_read_detects_suspended_sensor_instead_of_returning_zero_motion(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;
    nrf_imu_sample_t sample;

    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_OK);

    fake.regs[TEST_BMI160_REG_PMU_STATUS] = 0U;
    memset(&fake.regs[TEST_BMI160_REG_GYRO_DATA], 0, 12U);
    memset(&sample, 0x55, sizeof(sample));
    assert(nrf_bmi160_read_imu(&device, &sample) == NRF_BMI160_ERR_PMU);
    assert_sample_zero(&sample);
    assert(!device.initialized);
    assert(device.state == NRF_BMI160_RECOVER);
    assert(nrf_bmi160_read_imu(&device, &sample) ==
           NRF_BMI160_ERR_NOT_INITIALIZED);

    /* Re-init brings it back. */
    assert(nrf_bmi160_init(
               &device,
               &bus,
               NRF_BMI160_DEFAULT_ADDRESS,
               fake_delay_ms,
               &fake) == NRF_BMI160_OK);
    put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 6U], 16384);
    assert(nrf_bmi160_read_imu(&device, &sample) == NRF_BMI160_OK);
    assert(sample.accel_mg[0] == 1000);
}

static void test_state_machine_initialization_and_recovery(void)
{
    fake_bmi160_bus_t baseline;
    seed_sensor(&baseline);
    nrf_sensor_bus_t bus = make_bus(&baseline);
    nrf_bmi160_t device;
    baseline.observed_device = &device;
    assert(nrf_bmi160_init_observed(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                           fake_delay_ms, &baseline, observe_state, &baseline) == NRF_BMI160_OK);
    assert(baseline.transition_count == 4U);
    assert(baseline.transitions[0] == NRF_BMI160_PROBE);
    assert(baseline.transitions[1] == NRF_BMI160_CONFIGURE);
    assert(baseline.transitions[2] == NRF_BMI160_VERIFY);
    assert(baseline.transitions[3] == NRF_BMI160_MEASURING);
    assert(device.state == NRF_BMI160_MEASURING);
    bool saw_probe = false, saw_configure = false, saw_verify = false;
    for (int i = 0; i < baseline.transfer_count; ++i) {
        saw_probe |= baseline.observed_states[i] == NRF_BMI160_PROBE;
        saw_configure |= baseline.observed_states[i] == NRF_BMI160_CONFIGURE;
        saw_verify |= baseline.observed_states[i] == NRF_BMI160_VERIFY;
    }
    assert(saw_probe && saw_configure && saw_verify);

    for (int failure = 1; failure <= baseline.transfer_count; ++failure) {
        fake_bmi160_bus_t fake;
        seed_sensor(&fake);
        bus = make_bus(&fake);
        fake.observed_device = &device;
        fake.fail_at_transfer = failure;
        assert(nrf_bmi160_init_observed(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                               fake_delay_ms, &fake, observe_state, &fake) == NRF_BMI160_ERR_BUS);
        assert(fake.transitions[fake.transition_count - 1U] == NRF_BMI160_RECOVER);
        assert(fake.observed_states[failure - 1] == baseline.observed_states[failure - 1]);
        assert(device.state == NRF_BMI160_RECOVER && !device.initialized);
        fake.fail_at_transfer = 0;
        fake.transfer_count = 0;
        fake.delay_count = 0;
        assert(nrf_bmi160_init(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                               fake_delay_ms, &fake) == NRF_BMI160_OK);
        assert(device.state == NRF_BMI160_MEASURING);
    }
}

static void test_async_initialization_respects_deadlines(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;
    uint64_t now = 100U;
    assert(nrf_bmi160_begin_async(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                                  observe_state, &fake) == NRF_BMI160_PENDING);
    assert(nrf_bmi160_step_async(&device, now) == NRF_BMI160_PENDING);
    assert(nrf_bmi160_step_async(&device, now) == NRF_BMI160_PENDING);
    const int transfers = fake.transfer_count;
    assert(nrf_bmi160_step_async(&device, now) == NRF_BMI160_PENDING);
    assert(fake.transfer_count == transfers && fake.delay_count == 0U);
    nrf_bmi160_result_t result = NRF_BMI160_PENDING;
    for (unsigned steps = 0U; result == NRF_BMI160_PENDING && steps < 20U; ++steps) {
        if (now < device.prepare_due_ms) now = device.prepare_due_ms;
        result = nrf_bmi160_step_async(&device, now);
    }
    assert(result == NRF_BMI160_OK && device.initialized);
    assert(now == 190U && fake.delay_count == 0U);
    assert(fake.transition_count == 4U && fake.transitions[3] == NRF_BMI160_MEASURING);
    const int total_transfers = fake.transfer_count;
    for (int failure = 1; failure <= total_transfers; ++failure) {
        seed_sensor(&fake);
        fake.fail_at_transfer = failure;
        assert(nrf_bmi160_begin_async(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                                      observe_state, &fake) == NRF_BMI160_PENDING);
        now = 0U;
        result = NRF_BMI160_PENDING;
        for (unsigned steps = 0U; result == NRF_BMI160_PENDING && steps < 20U; ++steps) {
            if (now < device.prepare_due_ms) now = device.prepare_due_ms;
            result = nrf_bmi160_step_async(&device, now);
        }
        assert(result == NRF_BMI160_ERR_BUS && device.state == NRF_BMI160_RECOVER);
        assert(!device.initialized && fake.delay_count == 0U);
    }
    seed_sensor(&fake);
    fake.fail_at_transfer = 1;
    assert(nrf_bmi160_begin_async(&device, &bus, NRF_BMI160_DEFAULT_ADDRESS,
                                  observe_state, &fake) == NRF_BMI160_PENDING);
    assert(nrf_bmi160_step_async(&device, 0U) == NRF_BMI160_ERR_BUS);
    assert(device.state == NRF_BMI160_RECOVER && fake.transitions[1] == NRF_BMI160_RECOVER);
}

int main(void)
{
    test_async_initialization_respects_deadlines();
    test_state_machine_initialization_and_recovery();
    test_read_detects_suspended_sensor_instead_of_returning_zero_motion();
    test_init_rejects_invalid_arguments();
    test_init_configures_expected_runtime_profile();
    test_init_rejects_wrong_chip_id_before_powerup();
    test_init_rejects_pmu_that_did_not_enter_normal_mode();
    test_init_propagates_transfer_failure();
    test_read_requires_initialized_device_and_clears_output();
    test_read_converts_motion_and_temperature_without_float();
    test_read_transfer_failure_does_not_leak_stale_sample();
    return 0;
}
