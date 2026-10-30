#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "main_barometer.h"
#include "freertos/task.h"
#include "nrf_baro_altitude.h"

#define REG_CHIP_ID 0xD0U
#define REG_CALIB 0x88U
#define REG_STATUS 0xF3U
#define REG_DATA 0xF7U
#define REG_CTRL_MEAS 0xF4U
#define REG_CONFIG 0xF5U
#define BMP_ID 0x58U

typedef struct {
    uint8_t address;
} fake_device_t;

typedef struct {
    uint8_t registers[128][256];
    bool responding[128];
    fake_device_t devices[128];
    uint8_t read_addresses[32];
    size_t read_count;
    uint8_t probed_addresses[16];
    size_t address_probe_count;
    unsigned get_handles_calls;
    bool probe_timeout_enabled;
    uint8_t probe_timeout_address;
    int64_t now_us;
    bool timeout_bmp_data;
    /* Opt-in: a BMP/BME280 outside normal mode returns the reset data (0x80000). */
    bool bmp_model_sleep;
    /* The shared bus is held by another owner: reads miss the owner lock. */
    bool bmp_bus_busy;
    /* 2026-10-27 capture: the writes of CTRL_MEAS/CONFIG are ACKed but the sensor keeps its reset values. */
    bool bmp_ignore_mode_writes;
    /* Normal mode never converts (data stays at the reset value); a forced conversion does. */
    bool bmp_normal_dead;
    bool bmp_no_conversion;
    bool bmp_timed_normal;
    struct {
        uint8_t reg;
        uint8_t value;
    } bmp_writes[16];
    size_t bmp_write_count;
    void *bus;
    main_barometer_t *observed;
    unsigned state_mask;
    unsigned bmp_transfer_count;
    unsigned bmp_fail_at;
    esp_err_t bmp_delete_error;
    main_bmp280_state_t bmp_failed_state;
} fake_sensor_bus_t;

static fake_sensor_bus_t s_fake;
extern char fake_barometer_log[8192];

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void set_bmp_raw_pressure(uint8_t address, uint32_t raw_pressure)
{
    uint8_t *data = &s_fake.registers[address][REG_DATA];
    data[0] = (uint8_t)(raw_pressure >> 12U);
    data[1] = (uint8_t)(raw_pressure >> 4U);
    data[2] = (uint8_t)(raw_pressure << 4U);
}

static void setup_bmp280(uint8_t address, uint8_t chip_id)
{
    static const main_barometer_bmp280_calibration_t calibration = {
        .dig_t1 = 27504U, .dig_t2 = 26435, .dig_t3 = -1000,
        .dig_p1 = 36477U, .dig_p2 = -10685, .dig_p3 = 3024,
        .dig_p4 = 2855, .dig_p5 = 140, .dig_p6 = -7,
        .dig_p7 = 15500, .dig_p8 = -14600, .dig_p9 = 6000,
    };
    s_fake.responding[address] = true;
    s_fake.registers[address][REG_CHIP_ID] = chip_id;
    uint8_t *cal = &s_fake.registers[address][REG_CALIB];
    put_le16(&cal[0], calibration.dig_t1);
    put_le16(&cal[2], (uint16_t)calibration.dig_t2);
    put_le16(&cal[4], (uint16_t)calibration.dig_t3);
    put_le16(&cal[6], calibration.dig_p1);
    put_le16(&cal[8], (uint16_t)calibration.dig_p2);
    put_le16(&cal[10], (uint16_t)calibration.dig_p3);
    put_le16(&cal[12], (uint16_t)calibration.dig_p4);
    put_le16(&cal[14], (uint16_t)calibration.dig_p5);
    put_le16(&cal[16], (uint16_t)calibration.dig_p6);
    put_le16(&cal[18], (uint16_t)calibration.dig_p7);
    put_le16(&cal[20], (uint16_t)calibration.dig_p8);
    put_le16(&cal[22], (uint16_t)calibration.dig_p9);

    const uint32_t raw_pressure = 415148U;
    const uint32_t raw_temperature = 519888U;
    uint8_t *data = &s_fake.registers[address][REG_DATA];
    set_bmp_raw_pressure(address, raw_pressure);
    data[3] = (uint8_t)(raw_temperature >> 12U);
    data[4] = (uint8_t)(raw_temperature >> 4U);
    data[5] = (uint8_t)(raw_temperature << 4U);
}

static void reset_fake(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    fake_barometer_log[0] = '\0';
}

esp_err_t sensor_bus_probe_address(uint8_t address)
{
    if (s_fake.observed != NULL && (address == 0x76U || address == 0x77U))
        s_fake.state_mask |= 1U << s_fake.observed->bmp280_state;
    if (s_fake.address_probe_count < sizeof(s_fake.probed_addresses)) {
        s_fake.probed_addresses[s_fake.address_probe_count++] = address;
    }
    if (s_fake.probe_timeout_enabled && address == s_fake.probe_timeout_address) {
        return ESP_ERR_TIMEOUT;
    }
    return s_fake.responding[address] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    ++s_fake.get_handles_calls;
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handles->bus = &s_fake.bus;
    handles->ina3221 = NULL;
    return ESP_OK;
}

esp_err_t sensor_bus_classify_transfer_error(uint8_t address, esp_err_t err)
{
    if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return s_fake.responding[address] ? err : ESP_ERR_NOT_FOUND;
}

i2c_bus_device_handle_t i2c_bus_device_create(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed_hz)
{
    assert(bus == &s_fake.bus);
    assert(speed_hz == SENSOR_BUS_I2C_SPEED_HZ);
    s_fake.devices[address].address = address;
    return &s_fake.devices[address];
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*device != NULL && s_fake.bmp_delete_error != ESP_OK) {
        return s_fake.bmp_delete_error;
    }
    *device = NULL;
    return ESP_OK;
}

static esp_err_t fake_read(
    i2c_bus_device_handle_t handle,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data)
{
    const uint8_t address = ((fake_device_t *)handle)->address;
    if (s_fake.observed != NULL && (address == 0x76U || address == 0x77U))
        s_fake.state_mask |= 1U << s_fake.observed->bmp280_state;
    if (s_fake.read_count < sizeof(s_fake.read_addresses)) {
        s_fake.read_addresses[s_fake.read_count++] = address;
    }
    if ((address == 0x76U || address == 0x77U) && s_fake.responding[address]) {
        ++s_fake.bmp_transfer_count;
        if (s_fake.bmp_fail_at == s_fake.bmp_transfer_count) {
            s_fake.bmp_failed_state = s_fake.observed->bmp280_state;
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!s_fake.responding[address]) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((address == 0x76U || address == 0x77U) &&
        mem_address == REG_DATA && s_fake.timeout_bmp_data) {
        return ESP_ERR_TIMEOUT;
    }
    if ((address == 0x76U || address == 0x77U) && s_fake.bmp_bus_busy) {
        return SENSOR_BUS_ERR_MUTEX_BUSY;
    }
    if ((address == 0x76U || address == 0x77U) && mem_address == REG_DATA && data_len == 6U &&
        s_fake.bmp_normal_dead && (s_fake.registers[address][REG_CTRL_MEAS] & 0x03U) == 0x03U) {
        static const uint8_t dead_data[6] = {0x80U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U};
        memcpy(data, dead_data, sizeof(dead_data));
        return ESP_OK;
    }
    if ((address == 0x76U || address == 0x77U) && mem_address == REG_DATA &&
        s_fake.bmp_model_sleep && data_len == 6U &&
        (s_fake.registers[address][REG_CTRL_MEAS] & 0x03U) != 0x03U) {
        static const uint8_t reset_data[6] = {0x80U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U};
        memcpy(data, reset_data, sizeof(reset_data));
        return ESP_OK;
    }
    if ((size_t)mem_address + data_len > sizeof(s_fake.registers[address])) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((address == 0x76U || address == 0x77U) && mem_address == REG_STATUS && data_len == 3U &&
        (s_fake.registers[address][REG_CTRL_MEAS] & 3U) == 1U && !s_fake.bmp_no_conversion) {
        s_fake.registers[address][REG_CTRL_MEAS] &= ~3U;
        s_fake.registers[address][REG_STATUS] = 0U;
    }
    if (s_fake.bmp_timed_normal && mem_address == REG_STATUS &&
        (s_fake.registers[address][REG_CTRL_MEAS] & 3U) == 3U)
        s_fake.registers[address][REG_STATUS] = ((s_fake.now_us / 1000 + 52) % 106) < 44 ? 8U : 0U;
    memcpy(data, &s_fake.registers[address][mem_address], data_len);
    if ((address == 0x76U || address == 0x77U) && !s_fake.bmp_no_conversion && !s_fake.bmp_normal_dead) {
        if (mem_address == REG_STATUS) s_fake.registers[address][REG_STATUS] = 0U;
        if (mem_address == REG_DATA && data[0] != 0x80U &&
            (s_fake.registers[address][REG_CTRL_MEAS] & 3U) == 3U)
            s_fake.registers[address][REG_STATUS] = 8U;
    }
    return ESP_OK;
}

esp_err_t i2c_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data)
{
    return fake_read(device, mem_address, 1U, data);
}

esp_err_t i2c_bus_read_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data)
{
    return fake_read(device, mem_address, data_len, data);
}

esp_err_t i2c_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data)
{
    return i2c_bus_write_bytes(device, mem_address, 1U, &data);
}

esp_err_t i2c_bus_write_bytes(
    i2c_bus_device_handle_t handle,
    uint8_t mem_address,
    size_t data_len,
    const uint8_t *data)
{
    const uint8_t address = ((fake_device_t *)handle)->address;
    if (s_fake.observed != NULL && (address == 0x76U || address == 0x77U))
        s_fake.state_mask |= 1U << s_fake.observed->bmp280_state;
    if ((address == 0x76U || address == 0x77U) && s_fake.responding[address]) {
        ++s_fake.bmp_transfer_count;
        if (s_fake.bmp_fail_at == s_fake.bmp_transfer_count) {
            s_fake.bmp_failed_state = s_fake.observed->bmp280_state;
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!s_fake.responding[address]) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((size_t)mem_address + data_len > sizeof(s_fake.registers[address])) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((address == 0x76U || address == 0x77U) && data_len == 1U &&
        s_fake.bmp_write_count < sizeof(s_fake.bmp_writes) / sizeof(s_fake.bmp_writes[0])) {
        s_fake.bmp_writes[s_fake.bmp_write_count].reg = mem_address;
        s_fake.bmp_writes[s_fake.bmp_write_count].value = data[0];
        ++s_fake.bmp_write_count;
    }
    if ((address == 0x76U || address == 0x77U) && s_fake.bmp_ignore_mode_writes &&
        (mem_address == REG_CTRL_MEAS || mem_address == REG_CONFIG)) {
        return ESP_OK; /* ACKed, not kept */
    }
    memcpy(&s_fake.registers[address][mem_address], data, data_len);
    if ((address == 0x76U || address == 0x77U) && mem_address == REG_CTRL_MEAS &&
        data_len == 1U && (data[0] & 3U) == 3U && !s_fake.bmp_normal_dead && !s_fake.bmp_no_conversion)
        s_fake.registers[address][REG_STATUS] = 8U;
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks_to_wait)
{
    s_fake.now_us += (int64_t)ticks_to_wait * INT64_C(10000);
}

int64_t esp_timer_get_time(void)
{
    return s_fake.now_us;
}

static void test_bmp280_compensation_datasheet_vector(void)
{
    const main_barometer_bmp280_calibration_t calibration = {
        .dig_t1 = 27504U, .dig_t2 = 26435, .dig_t3 = -1000,
        .dig_p1 = 36477U, .dig_p2 = -10685, .dig_p3 = 3024,
        .dig_p4 = 2855, .dig_p5 = 140, .dig_p6 = -7,
        .dig_p7 = 15500, .dig_p8 = -14600, .dig_p9 = 6000,
    };
    uint32_t pressure_pa = 0U;
    int32_t temperature_mc = 0;
    assert(main_barometer_bmp280_compensate(
        415148U, 519888U, &calibration, &pressure_pa, &temperature_mc));
    assert(temperature_mc == 25080);
    assert(pressure_pa >= 100652U && pressure_pa <= 100654U);
}

static void poll_range(
    main_barometer_t *barometer,
    uint64_t from_ms,
    uint64_t to_ms);

static void test_established_reference_holds_through_sensor_failure_and_restores(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(130U) == ESP_OK);
    poll_range(&barometer, 0U, 3400U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);

    set_bmp_raw_pressure(0x77U, 414000U);
    poll_range(&barometer, 3600U, 4800U);
    assert(barometer.reading.bmp280_valid);
    const int32_t held_z_mm = barometer.reading.relative_z_mm;
    assert(held_z_mm != 0);
    assert(held_z_mm == barometer.reading.filtered_altitude_mm - barometer.height_reference_altitude_mm);
    const uint64_t last_good_ms = barometer.height_reference_last_good_ms;
    assert(last_good_ms >= 4600U);

    /* The sensor stops answering: the calibrated Z is held, not discarded. */
    s_fake.timeout_bmp_data = true;
    uint64_t now_ms = 5000U;
    for (; now_ms < last_good_ms + 590000U; now_ms += 1000U) {
        (void)main_barometer_poll(&barometer, now_ms);
        assert(!barometer.reading.bmp280_valid);
        assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
        assert(barometer.reading.height_reference_generation == 130U);
        assert(barometer.reading.relative_z_mm == held_z_mm);
    }

    /* A long outage stops the hold: weather drift no longer cancels against the REMOTE nodes. */
    for (; now_ms <= last_good_ms + 601000U; now_ms += 1000U) {
        (void)main_barometer_poll(&barometer, now_ms);
    }
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    assert(barometer.reading.relative_z_mm == 0);

    /* The sensor returns: the stored reference is restored from a fresh sample. */
    s_fake.timeout_bmp_data = false;
    for (unsigned i = 0U; i < 80U &&
         barometer.reading.height_reference_state != MAIN_BAROMETER_REFERENCE_VALID; ++i, now_ms += 1000U) {
        (void)main_barometer_poll(&barometer, now_ms);
    }
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(barometer.reading.height_reference_generation == 130U);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.relative_z_mm ==
           barometer.reading.filtered_altitude_mm - barometer.height_reference_altitude_mm);
    main_barometer_close(&barometer);
}

static void test_transient_failure_during_capture_still_invalidates_it(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.timeout_bmp_data = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(131U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    (void)main_barometer_poll(&barometer, 200U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    /* No reference was ever established, so a returning sensor must not invent one. */
    s_fake.timeout_bmp_data = false;
    poll_range(&barometer, 6000U, 9000U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    assert(barometer.reading.relative_z_mm == 0);
    main_barometer_close(&barometer);
}

static void test_bmp280_probes_preferred_then_fallback_address(void)
{
    reset_fake();
    setup_bmp280(0x76U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    assert(barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid); /* first conversion still running */
    (void)main_barometer_poll(&barometer, 200U);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.bmp280_address == 0x76U);
    assert(barometer.reading.pressure_pa >= 100652U &&
           barometer.reading.pressure_pa <= 100654U);
    assert(barometer.reading.filtered_altitude_mm ==
           nrf_baro_pressure_to_altitude_mm(barometer.reading.pressure_pa));
    assert(s_fake.address_probe_count >= 2U);
    assert(s_fake.probed_addresses[0] == 0x77U);
    assert(s_fake.probed_addresses[1] == 0x76U);
    assert(s_fake.read_count > 0U);
    assert(s_fake.read_addresses[0] == 0x76U);
    main_barometer_close(&barometer);
}

static void test_bme280_accepted_as_bmp280(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    (void)main_barometer_poll(&barometer, 200U);
    assert(barometer.reading.bmp280_present);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.bme280);
    main_barometer_close(&barometer);
}

static void test_bmp280_rejects_wrong_chip_id(void)
{
    reset_fake();
    setup_bmp280(0x77U, 0x61U);
    setup_bmp280(0x76U, 0x61U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_INVALID_RESPONSE);
    assert(!barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_absent_bmp280_and_data_timeout_error_classification(void)
{
    main_barometer_t barometer;

    reset_fake();
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_NOT_FOUND);
    assert(!barometer.reading.bmp280_present);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.timeout_bmp_data = true;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_TIMEOUT);
    assert(!barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_absent_bmp280_retries_probe_only_for_five_seconds_then_reads_after_ack(void)
{
    reset_fake();
    main_barometer_t barometer;
    main_barometer_init(&barometer);

    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_NOT_FOUND);
    assert(s_fake.address_probe_count == 2U); /* BMP 0x77/0x76 */
    assert(s_fake.read_count == 0U);

    (void)main_barometer_poll(&barometer, 200U);
    (void)main_barometer_poll(&barometer, 4800U);
    assert(s_fake.address_probe_count == 2U);
    assert(s_fake.read_count == 0U);

    setup_bmp280(0x76U, BMP_ID);
    (void)main_barometer_poll(&barometer, 5000U);
    (void)main_barometer_poll(&barometer, 5200U);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.bmp280_address == 0x76U);
    assert(s_fake.address_probe_count > 2U);
    assert(s_fake.read_count > 0U);
    main_barometer_close(&barometer);
}

static void test_probe_timeout_skips_full_read_transaction(void)
{
    reset_fake();
    s_fake.probe_timeout_enabled = true;
    s_fake.probe_timeout_address = 0x77U;
    main_barometer_t barometer;
    main_barometer_init(&barometer);

    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_TIMEOUT);
    assert(s_fake.read_count == 0U);
    const size_t probes_after_timeout = s_fake.address_probe_count;
    (void)main_barometer_poll(&barometer, 200U);
    assert(s_fake.address_probe_count == probes_after_timeout);
    assert(s_fake.read_count == 0U);
    main_barometer_close(&barometer);
}

static void test_main_height_reference_stable_unstable_and_relative(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(91U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);    /* init only, no sample yet */
    (void)main_barometer_poll(&barometer, 1000U); /* capture window starts */
    (void)main_barometer_poll(&barometer, 2000U);
    (void)main_barometer_poll(&barometer, 3000U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(barometer.reading.height_reference_generation == 91U);
    assert(barometer.reading.relative_z_mm == 0);

    const int32_t reference_altitude_mm = barometer.reading.filtered_altitude_mm;
    set_bmp_raw_pressure(0x77U, 414000U);
    (void)main_barometer_poll(&barometer, 3200U);
    assert(barometer.reading.relative_z_mm ==
           barometer.reading.filtered_altitude_mm - reference_altitude_mm);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(92U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    (void)main_barometer_poll(&barometer, 1000U);
    set_bmp_raw_pressure(0x77U, 418000U);
    (void)main_barometer_poll(&barometer, 2000U);
    (void)main_barometer_poll(&barometer, 3000U);
    assert(barometer.reading.height_reference_state ==
           MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE);
    assert(barometer.reading.height_reference_generation == 92U);
    assert(barometer.reading.relative_z_mm == 0);
    main_barometer_close(&barometer);
}

static void test_main_height_reference_sensor_invalid_and_generation_validation(void)
{
    main_barometer_t barometer;
    assert(main_barometer_request_height_reference_capture(0U) == ESP_ERR_INVALID_ARG);

    reset_fake();
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(93U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    assert(barometer.reading.height_reference_state ==
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    assert(barometer.reading.height_reference_generation == 93U);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.timeout_bmp_data = true;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(94U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    (void)main_barometer_poll(&barometer, 200U);
    assert(barometer.reading.height_reference_state ==
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    assert(barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_first_conversion_pending_is_not_a_bus_error(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    /* Data registers still hold the reset value until a conversion ends. */
    set_bmp_raw_pressure(0x76U, 0x80000U);
    s_fake.registers[0x76U][REG_DATA + 3U] = 0x80U;
    s_fake.registers[0x76U][REG_DATA + 4U] = 0x00U;
    s_fake.registers[0x76U][REG_DATA + 5U] = 0x00U;
    main_barometer_t barometer;
    main_barometer_init(&barometer);

    /* The init poll must not read the data registers in the same call. */
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    assert(barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid);
    const size_t reads_after_init = s_fake.read_count;

    /* A still-pending conversion is "not finished", retried without backoff. */
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_NOT_FINISHED);
    assert(!barometer.reading.bmp280_valid);
    assert(barometer.bmp280_retry_after_ms == 200U);
    assert(s_fake.read_count > reads_after_init);

    /* Real data arrives: the very next poll period reads it. */
    const uint32_t raw_temperature = 519888U;
    set_bmp_raw_pressure(0x76U, 415148U);
    s_fake.registers[0x76U][REG_DATA + 3U] = (uint8_t)(raw_temperature >> 12U);
    s_fake.registers[0x76U][REG_DATA + 4U] = (uint8_t)(raw_temperature >> 4U);
    s_fake.registers[0x76U][REG_DATA + 5U] = (uint8_t)(raw_temperature << 4U);
    assert(main_barometer_poll(&barometer, 400U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_boot_polling_does_not_touch_i2c_before_preflight(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(NULL, 0U) == ESP_ERR_INVALID_ARG);
    assert(main_barometer_request_height_reference_capture(0U) == ESP_ERR_INVALID_ARG);
    /* No polling requested and no bus yet: the owning task has no deadline to meet. */
    assert(main_barometer_ms_until_poll(NULL, 0U, 700U) == 700U);
    assert(main_barometer_ms_until_poll(&barometer, 0U, 700U) == 700U);
    for (uint64_t now_ms = 0U; now_ms <= 10000U; now_ms += 1000U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_OK);
        assert(s_fake.get_handles_calls == 0U);
        assert(s_fake.address_probe_count == 0U);
        assert(s_fake.read_count == 0U);
        assert(!barometer.reading.bmp280_present);
    }
    main_barometer_request_polling();
    main_barometer_request_polling();
    assert(s_fake.get_handles_calls == 0U);
    assert(s_fake.address_probe_count == 0U);
    assert(s_fake.read_count == 0U);
    /* Requested, but the bus is not open yet: still no deadline. */
    assert(main_barometer_ms_until_poll(&barometer, 11000U, 700U) == 700U);
    assert(main_barometer_poll(&barometer, 11000U) == ESP_OK);
    assert(s_fake.get_handles_calls == 1U);
    /* The task must wake for the next poll, never later and never past its own cap. */
    assert(barometer.next_bmp280_poll_ms > 11000U);
    const uint64_t due_ms = barometer.next_bmp280_poll_ms;
    assert(main_barometer_ms_until_poll(&barometer, 11000U, 700U) ==
           (due_ms - 11000U < 700U ? (uint32_t)(due_ms - 11000U) : 700U));
    assert(main_barometer_ms_until_poll(&barometer, 11000U, 1U) == 1U);
    assert(main_barometer_ms_until_poll(&barometer, due_ms, 700U) == 1U);
    assert(main_barometer_ms_until_poll(&barometer, due_ms + 500U, 700U) == 1U);
    assert(barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid);
    assert(main_barometer_poll(&barometer, 12000U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.pressure_pa > 0U);
    assert(barometer.reading.height_reference_state ==
        MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED);
    main_barometer_close(&barometer);
}

/* A power loss or soft reset returns the sensor to sleep mode with default registers. */
static void bmp_lose_configuration(uint8_t address)
{
    s_fake.registers[address][REG_CTRL_MEAS] = 0U;
    s_fake.registers[address][REG_CONFIG] = 0U;
}

static void poll_range(
    main_barometer_t *barometer,
    uint64_t from_ms,
    uint64_t to_ms)
{
    for (uint64_t now_ms = from_ms; now_ms <= to_ms; now_ms += 200U) {
        (void)main_barometer_poll(barometer, now_ms);
    }
}

static void test_sensor_power_cycle_is_rearmed_after_it_acks_again(void)
{
    reset_fake();
    main_barometer_request_polling(); /* polling is a process-wide opt-in */
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_model_sleep = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    poll_range(&barometer, 0U, 400U);
    assert(barometer.reading.bmp280_valid);

    /* The sensor drops off the bus, then comes back reset (sleep mode). */
    s_fake.responding[0x76U] = false;
    assert(main_barometer_poll(&barometer, 600U) == ESP_ERR_NOT_FOUND);
    assert(!barometer.reading.bmp280_valid);
    s_fake.responding[0x76U] = true;
    bmp_lose_configuration(0x76U);

    poll_range(&barometer, 800U, 6600U);
    assert(barometer.reading.bmp280_valid);
    assert(s_fake.registers[0x76U][REG_CTRL_MEAS] == 0x57U);
    assert(s_fake.registers[0x76U][REG_CONFIG] == 0x30U);
    main_barometer_close(&barometer);
}

static void test_silent_sensor_reset_is_rearmed_after_bounded_pending_polls(void)
{
    reset_fake();
    main_barometer_request_polling(); /* polling is a process-wide opt-in */
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_model_sleep = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    poll_range(&barometer, 0U, 400U);
    assert(barometer.reading.bmp280_valid);

    bmp_lose_configuration(0x76U);
    for (uint64_t now_ms = 600U; now_ms <= 1400U; now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
        assert(!barometer.reading.bmp280_valid);
    }
    /* 5th pending poll re-arms the mode; the next poll period reads real data. */
    assert(s_fake.registers[0x76U][REG_CTRL_MEAS] == 0x57U);
    assert(main_barometer_poll(&barometer, 1600U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_pending_conversion_does_not_add_probe_traffic(void)
{
    reset_fake();
    main_barometer_request_polling(); /* polling is a process-wide opt-in */
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_model_sleep = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    poll_range(&barometer, 0U, 400U);
    bmp_lose_configuration(0x76U);

    const size_t probes_before = s_fake.address_probe_count;
    for (uint64_t now_ms = 600U; now_ms <= 1000U; now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
    }
    assert(s_fake.address_probe_count == probes_before);
    main_barometer_close(&barometer);
}

static void test_transient_pending_does_not_kill_height_reference_capture(void)
{
    reset_fake();
    main_barometer_request_polling(); /* polling is a process-wide opt-in */
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(95U) == ESP_OK);
    poll_range(&barometer, 0U, 200U); /* init, then a first valid sample */
    assert(barometer.reading.bmp280_valid);

    /* One conversion not finished yet: not a sensor failure. */
    uint8_t saved[6];
    memcpy(saved, &s_fake.registers[0x77U][REG_DATA], sizeof(saved));
    set_bmp_raw_pressure(0x77U, 0x80000U);
    assert(main_barometer_poll(&barometer, 400U) == ESP_ERR_NOT_FINISHED);
    assert(barometer.reading.height_reference_state !=
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    memcpy(&s_fake.registers[0x77U][REG_DATA], saved, sizeof(saved));

    poll_range(&barometer, 600U, 3600U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(barometer.reading.height_reference_generation == 95U);
    main_barometer_close(&barometer);
}

static void test_permanently_pending_sensor_invalidates_capture_after_bound(void)
{
    reset_fake();
    main_barometer_request_polling(); /* polling is a process-wide opt-in */
    setup_bmp280(0x77U, BMP_ID);
    set_bmp_raw_pressure(0x77U, 0x80000U); /* never converts, even after re-arm */
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(96U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);

    uint64_t now_ms = 200U;
    for (unsigned pending = 1U; pending <= 14U; ++pending, now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
        assert(barometer.reading.height_reference_state ==
               MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED);
    }
    assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
    assert(barometer.reading.height_reference_state ==
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    main_barometer_close(&barometer);
}

/*
 * SENSOR_BUS_ERR_MUTEX_BUSY means another task owns the shared bus (the field
 * log prints it as "ERROR"). It says nothing about the sensor: it must not force
 * a re-probe, and must not invalidate a height-reference capture.
 */
static void test_bus_busy_is_not_a_sensor_failure(void)
{
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_model_sleep = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    poll_range(&barometer, 0U, 400U);
    assert(barometer.reading.bmp280_valid);

    const size_t probes_before = s_fake.address_probe_count;
    s_fake.bmp_bus_busy = true;
    for (uint64_t now_ms = 600U; now_ms <= 1000U; now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == SENSOR_BUS_ERR_MUTEX_BUSY);
        assert(!barometer.reading.bmp280_valid);
    }
    assert(s_fake.address_probe_count == probes_before);

    s_fake.bmp_bus_busy = false;
    assert(main_barometer_poll(&barometer, 1200U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    assert(s_fake.address_probe_count == probes_before);
    main_barometer_close(&barometer);
}

static void test_bus_busy_does_not_kill_height_reference_capture(void)
{
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(97U) == ESP_OK);
    poll_range(&barometer, 0U, 400U);
    assert(barometer.reading.bmp280_valid);

    s_fake.bmp_bus_busy = true;
    assert(main_barometer_poll(&barometer, 600U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(main_barometer_poll(&barometer, 800U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(barometer.reading.height_reference_state !=
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    s_fake.bmp_bus_busy = false;

    poll_range(&barometer, 1000U, 4000U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(barometer.reading.height_reference_generation == 97U);
    main_barometer_close(&barometer);
}

static void test_persistent_bus_busy_invalidates_capture_after_bound(void)
{
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(98U) == ESP_OK);
    poll_range(&barometer, 0U, 400U);

    s_fake.bmp_bus_busy = true;
    uint64_t now_ms = 600U;
    for (unsigned busy = 1U; busy <= 14U; ++busy, now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == SENSOR_BUS_ERR_MUTEX_BUSY);
        assert(barometer.reading.height_reference_state !=
               MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    }
    assert(main_barometer_poll(&barometer, now_ms) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(barometer.reading.height_reference_state ==
           MAIN_BAROMETER_REFERENCE_SENSOR_INVALID);
    main_barometer_close(&barometer);
}

static void test_bmp280_is_armed_through_sleep_and_verified(void)
{
    reset_fake();
    setup_bmp280(0x76U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    /* CONFIG is only writable in sleep mode: sleep, config, then the measuring mode. */
    assert(s_fake.bmp_write_count == 3U);
    assert(s_fake.bmp_writes[0].reg == REG_CTRL_MEAS && s_fake.bmp_writes[0].value == 0x00U);
    assert(s_fake.bmp_writes[1].reg == REG_CONFIG && s_fake.bmp_writes[1].value == 0x30U);
    assert(s_fake.bmp_writes[2].reg == REG_CTRL_MEAS && s_fake.bmp_writes[2].value == 0x57U);
    assert(barometer.reading.bmp280_present);
    main_barometer_close(&barometer);
}

static void test_bmp280_that_acks_but_keeps_the_reset_state_is_not_started(void)
{
    /* The 2026-10-27 capture: every write ACKed, the data registers never left 80 00 00 80 00 00. */
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x76U, BMP_ID);
    s_fake.bmp_ignore_mode_writes = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_INVALID_STATE);
    assert(!barometer.reading.bmp280_present && !barometer.reading.bmp280_valid);
    assert(!barometer.bmp280_initialized);
    main_barometer_close(&barometer);
}

static void test_normal_mode_that_never_converts_falls_back_to_forced_conversions(void)
{
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x76U, BMP_ID);
    s_fake.bmp_normal_dead = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);

    uint64_t now_ms = 200U;
    for (unsigned polls = 1U; polls < 10U; ++polls, now_ms += 200U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
        assert(!barometer.bmp280_forced && !barometer.reading.bmp280_valid);
    }
    /* Fallback starts before the final 15-poll capture failure bound. */
    assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
    assert(barometer.bmp280_forced);
    now_ms += 200U;
    assert(main_barometer_poll(&barometer, now_ms) == ESP_ERR_NOT_FINISHED);
    assert(s_fake.registers[0x76U][REG_CTRL_MEAS] == 0x55U);
    assert(main_barometer_poll(&barometer, now_ms + 50U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    assert(s_fake.registers[0x76U][REG_CTRL_MEAS] == 0x54U); /* conversion returned to sleep */
    main_barometer_close(&barometer);
    assert(!barometer.bmp280_forced);
}

static void test_bmp_state_machine_reprobes_identity_after_read_failure(void)
{
    reset_fake();
    main_barometer_request_polling();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    s_fake.observed = &barometer;
    assert(barometer.bmp280_state == MAIN_BMP280_PROBE);
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    assert((s_fake.state_mask & ((1U << MAIN_BMP280_PROBE) |
        (1U << MAIN_BMP280_CONFIGURE) | (1U << MAIN_BMP280_VERIFY))) ==
        ((1U << MAIN_BMP280_PROBE) | (1U << MAIN_BMP280_CONFIGURE) |
         (1U << MAIN_BMP280_VERIFY)));
    assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
    assert(main_barometer_poll(&barometer, 200U) == ESP_OK);
    s_fake.bmp_bus_busy = true;
    assert(main_barometer_poll(&barometer, 400U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
    s_fake.bmp_bus_busy = false;
    s_fake.timeout_bmp_data = true;
    assert(main_barometer_poll(&barometer, 600U) == ESP_ERR_TIMEOUT);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    assert(!barometer.reading.bmp280_valid);
    s_fake.timeout_bmp_data = false;
    s_fake.registers[0x77U][REG_CHIP_ID] = 0x42U;
    const size_t writes = s_fake.bmp_write_count;
    assert(main_barometer_poll(&barometer, 800U) == ESP_ERR_TIMEOUT);
    assert(s_fake.bmp_write_count == writes);
    assert(main_barometer_poll(&barometer, 5600U) == ESP_ERR_INVALID_RESPONSE);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    assert(s_fake.bmp_write_count == writes);
    s_fake.registers[0x77U][REG_CHIP_ID] = BMP_ID;
    assert(main_barometer_poll(&barometer, 5800U) == ESP_OK);
    assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
    assert(main_barometer_poll(&barometer, 6000U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_bmp_state_machine_recovers_from_each_setup_transfer_failure(void)
{
    static const main_bmp280_state_t phase[] = {
        MAIN_BMP280_PROBE, MAIN_BMP280_PROBE,
        MAIN_BMP280_CONFIGURE, MAIN_BMP280_CONFIGURE, MAIN_BMP280_CONFIGURE,
        MAIN_BMP280_VERIFY,
    };
    for (unsigned failure = 1U; failure <= sizeof(phase) / sizeof(phase[0]); ++failure) {
        reset_fake();
        setup_bmp280(0x77U, BMP_ID);
        main_barometer_t barometer;
        main_barometer_init(&barometer);
        s_fake.observed = &barometer;
        s_fake.bmp_fail_at = failure;
        assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_TIMEOUT);
        assert(s_fake.bmp_failed_state == phase[failure - 1U]);
        assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
        assert(!barometer.bmp280_initialized && !barometer.reading.bmp280_valid);
        const unsigned transfers = s_fake.bmp_transfer_count;
        s_fake.bmp_fail_at = 0U;
        assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_TIMEOUT);
        assert(s_fake.bmp_transfer_count == transfers);
        assert(main_barometer_poll(&barometer, 5000U) == ESP_OK);
        assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
        assert(!barometer.reading.bmp280_valid);
        assert(main_barometer_poll(&barometer, 5200U) == ESP_OK);
        assert(barometer.reading.bmp280_valid);
        main_barometer_close(&barometer);
    }
}

static void test_recovery_retains_handle_when_bus_lock_prevents_delete(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    s_fake.timeout_bmp_data = true;
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_TIMEOUT);
    s_fake.timeout_bmp_data = false;
    const i2c_bus_device_handle_t old = barometer.bmp280;
    const size_t probes = s_fake.address_probe_count;
    s_fake.bmp_delete_error = SENSOR_BUS_ERR_MUTEX_BUSY;
    assert(main_barometer_poll(&barometer, 5200U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(barometer.bmp280 == old);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    assert(s_fake.address_probe_count == probes);
    s_fake.bmp_delete_error = ESP_OK;
    assert(main_barometer_poll(&barometer, 5400U) == ESP_OK);
    assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
    main_barometer_close(&barometer);
}

static void test_failed_candidate_is_retained_until_delete_succeeds(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.bmp_ignore_mode_writes = true;
    s_fake.bmp_delete_error = SENSOR_BUS_ERR_MUTEX_BUSY;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(barometer.bmp280 != NULL);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    const size_t probes = s_fake.address_probe_count;
    assert(main_barometer_poll(&barometer, 200U) == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(s_fake.address_probe_count == probes);
    s_fake.bmp_delete_error = ESP_OK;
    s_fake.bmp_ignore_mode_writes = false;
    assert(main_barometer_poll(&barometer, 400U) == ESP_OK);
    assert(barometer.bmp280_state == MAIN_BMP280_MEASURING);
    main_barometer_close(&barometer);
}

static void test_fallback_preserves_capture_and_does_not_sleep(void)
{
    reset_fake();
    setup_bmp280(0x76U, BMP_ID);
    s_fake.bmp_normal_dead = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(117U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    for (uint64_t now = 200U; now <= 3000U && !barometer.bmp280_forced; now += 200U)
        (void)main_barometer_poll(&barometer, now);
    assert(barometer.bmp280_forced);
    assert(barometer.height_reference_capture_active);
    const int64_t before = s_fake.now_us;
    (void)main_barometer_poll(&barometer, 3200U);
    assert(s_fake.now_us == before);
    poll_range(&barometer, 3400U, 6400U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(barometer.reading.height_reference_generation == 117U);
    main_barometer_close(&barometer);
}

static void test_normal_without_conversion_evidence_rejects_old_registers(void)
{
    reset_fake();
    setup_bmp280(0x76U, BMP_ID);
    s_fake.bmp_no_conversion = true;
    main_barometer_request_polling();
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    /* Registers still contain a plausible earlier sample, but no conversion ran. */
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_NOT_FINISHED);
    assert(!barometer.reading.bmp280_valid);
    main_barometer_close(&barometer);
}

static void test_forced_conversion_timeout_discards_state(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    barometer.bmp280_forced = true;
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_NOT_FINISHED);
    s_fake.bmp_no_conversion = true; /* mode remains forced, measuring never asserts */
    assert(main_barometer_poll(&barometer, 250U) == ESP_ERR_NOT_FINISHED);
    assert(main_barometer_poll(&barometer, 500U) == ESP_ERR_TIMEOUT);
    assert(!barometer.reading.bmp280_valid && !barometer.bmp280_conversion_pending);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    main_barometer_close(&barometer);
}

static void test_normal_sample_is_consumed_once(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    s_fake.bmp_no_conversion = true; /* initial conversion completes; no later one starts */
    s_fake.registers[0x76U][REG_STATUS] = 0U;
    assert(main_barometer_poll(&barometer, 200U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    const uint32_t pressure = barometer.reading.pressure_pa;
    assert(pressure != 0U);
    assert(main_barometer_poll(&barometer, 400U) == ESP_ERR_NOT_FINISHED);
    assert(!barometer.reading.bmp280_valid);
    /* Identical data becomes valid again only after another witnessed conversion. */
    s_fake.registers[0x76U][REG_STATUS] = 8U;
    assert(main_barometer_poll(&barometer, 600U) == ESP_ERR_NOT_FINISHED);
    s_fake.registers[0x76U][REG_STATUS] = 0U;
    assert(main_barometer_poll(&barometer, 800U) == ESP_OK);
    assert(barometer.reading.bmp280_valid && barometer.reading.pressure_pa == pressure);
    main_barometer_close(&barometer);
}

static void test_forced_ack_without_applied_command_is_not_a_sample(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    barometer.bmp280_forced = true;
    s_fake.bmp_ignore_mode_writes = true;
    assert(main_barometer_poll(&barometer, 200U) == ESP_ERR_INVALID_STATE);
    assert(!barometer.reading.bmp280_valid);
    assert(barometer.bmp280_state == MAIN_BMP280_RECOVER);
    assert(!barometer.bmp280_conversion_pending && !barometer.bmp280_conversion_seen);
    main_barometer_close(&barometer);
}

static void test_valid_reference_survives_forced_fallback(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(118U) == ESP_OK);
    poll_range(&barometer, 0U, 2400U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    s_fake.bmp_normal_dead = true;
    for (uint64_t now = 2600U; now <= 6600U; now += 200U) {
        (void)main_barometer_poll(&barometer, now);
        assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    }
    assert(barometer.bmp280_forced && barometer.reading.height_reference_generation == 118U);
    main_barometer_close(&barometer);
}

static void test_normal_conversion_schedule_avoids_busy_aliasing(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_timed_normal = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_request_height_reference_capture(119U) == ESP_OK);
    (void)main_barometer_poll(&barometer, 0U);
    unsigned fresh = 0U;
    for (uint64_t now = barometer.next_bmp280_poll_ms; now <= 4000U;
         now = barometer.next_bmp280_poll_ms) {
        s_fake.now_us = (int64_t)now * 1000;
        assert(main_barometer_poll(&barometer, now) != ESP_ERR_TIMEOUT);
        if (barometer.reading.bmp280_valid) ++fresh;
        assert(!barometer.bmp280_forced);
    }
    assert(fresh >= 10U);
    assert(barometer.reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    main_barometer_close(&barometer);
}

static void test_reset_data_log_reports_actual_bytes_and_is_rate_limited(void)
{
    for (unsigned both_channels = 0U; both_channels < 2U; ++both_channels) {
        reset_fake();
        setup_bmp280(0x76U, 0x60U);
        set_bmp_raw_pressure(0x76U, 0x80000U);
        if (both_channels != 0U) {
            const uint8_t reset_temperature[3] = {0x80U, 0U, 0U};
            memcpy(&s_fake.registers[0x76U][REG_DATA + 3U], reset_temperature, 3U);
        }
        main_barometer_t barometer;
        main_barometer_init(&barometer);
        main_barometer_request_polling();
        (void)main_barometer_poll(&barometer, 0U);
        fake_barometer_log[0] = '\0';
        poll_range(&barometer, 200U, 800U);
        assert(strstr(fake_barometer_log, "data still at the reset value") == NULL);
        (void)main_barometer_poll(&barometer, 1000U);
        const char *expected = both_channels != 0U
            ? "data still at the reset value (80 00 00 80 00 00)"
            : "data still at the reset value (80 00 00 7e ed 00)";
        const char *message = strstr(fake_barometer_log, expected);
        assert(message != NULL);
        (void)main_barometer_poll(&barometer, 1200U);
        assert(strstr(message + strlen(expected), "data still at the reset value") == NULL);
        main_barometer_close(&barometer);
    }
}

static void test_unwitnessed_conversion_does_not_log_reset_bytes(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    s_fake.bmp_no_conversion = true;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    main_barometer_request_polling();
    poll_range(&barometer, 0U, 1000U);
    assert(strstr(fake_barometer_log, "no fresh conversion after 5 polls: re-arming") != NULL);
    assert(strstr(fake_barometer_log, "data still at the reset value") == NULL);
    main_barometer_close(&barometer);
}

int main(void)
{
    test_boot_polling_does_not_touch_i2c_before_preflight();
    test_reset_data_log_reports_actual_bytes_and_is_rate_limited();
    test_unwitnessed_conversion_does_not_log_reset_bytes();
    test_normal_conversion_schedule_avoids_busy_aliasing();
    test_valid_reference_survives_forced_fallback();
    test_forced_ack_without_applied_command_is_not_a_sample();
    test_normal_sample_is_consumed_once();
    test_forced_conversion_timeout_discards_state();
    test_normal_without_conversion_evidence_rejects_old_registers();
    test_fallback_preserves_capture_and_does_not_sleep();
    test_bmp_state_machine_reprobes_identity_after_read_failure();
    test_bmp_state_machine_recovers_from_each_setup_transfer_failure();
    test_recovery_retains_handle_when_bus_lock_prevents_delete();
    test_failed_candidate_is_retained_until_delete_succeeds();
    test_bmp280_compensation_datasheet_vector();
    test_bmp280_probes_preferred_then_fallback_address();
    test_bme280_accepted_as_bmp280();
    test_bmp280_rejects_wrong_chip_id();
    test_absent_bmp280_and_data_timeout_error_classification();
    test_absent_bmp280_retries_probe_only_for_five_seconds_then_reads_after_ack();
    test_probe_timeout_skips_full_read_transaction();
    test_main_height_reference_stable_unstable_and_relative();
    test_established_reference_holds_through_sensor_failure_and_restores();
    test_transient_failure_during_capture_still_invalidates_it();
    test_main_height_reference_sensor_invalid_and_generation_validation();
    test_first_conversion_pending_is_not_a_bus_error();
    test_sensor_power_cycle_is_rearmed_after_it_acks_again();
    test_silent_sensor_reset_is_rearmed_after_bounded_pending_polls();
    test_pending_conversion_does_not_add_probe_traffic();
    test_transient_pending_does_not_kill_height_reference_capture();
    test_permanently_pending_sensor_invalidates_capture_after_bound();
    test_bmp280_is_armed_through_sleep_and_verified();
    test_bmp280_that_acks_but_keeps_the_reset_state_is_not_started();
    test_normal_mode_that_never_converts_falls_back_to_forced_conversions();
    test_bus_busy_is_not_a_sensor_failure();
    test_bus_busy_does_not_kill_height_reference_capture();
    test_persistent_bus_busy_invalidates_capture_after_bound();
    return 0;
}
