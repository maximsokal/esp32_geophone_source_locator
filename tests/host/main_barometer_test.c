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
#define REG_DATA 0xF7U
#define BMP_ID 0x58U
#define AHT_ADDRESS 0x38U

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
    uint8_t aht_frame[7];
    uint8_t aht_status;
    unsigned aht_busy_reads;
    unsigned aht_frame_reads;
    TickType_t last_delay_ticks;
    int64_t now_us;
    uint32_t delay_extra_us;
    bool timeout_bmp_data;
    void *bus;
} fake_sensor_bus_t;

static fake_sensor_bus_t s_fake;

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

static void setup_aht20(void)
{
    s_fake.responding[AHT_ADDRESS] = true;
    s_fake.aht_status = 0x08U;
    s_fake.aht_frame[0] = 0x08U;
    /* 50.000% RH and 0 C. */
    s_fake.aht_frame[1] = 0x80U;
    s_fake.aht_frame[2] = 0x00U;
    s_fake.aht_frame[3] = 0x04U;
    s_fake.aht_frame[4] = 0x00U;
    s_fake.aht_frame[5] = 0x00U;
    s_fake.aht_frame[6] = main_barometer_aht20_crc8(s_fake.aht_frame, 6U);
}

static void reset_fake(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    setup_aht20();
}

esp_err_t sensor_bus_probe_address(uint8_t address)
{
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
    if (s_fake.read_count < sizeof(s_fake.read_addresses)) {
        s_fake.read_addresses[s_fake.read_count++] = address;
    }
    if (!s_fake.responding[address]) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((address == 0x76U || address == 0x77U) &&
        mem_address == REG_DATA && s_fake.timeout_bmp_data) {
        return ESP_ERR_TIMEOUT;
    }
    if (address == AHT_ADDRESS && mem_address == NULL_I2C_MEM_ADDR) {
        if (data_len == 1U) {
            data[0] = s_fake.aht_status;
            return ESP_OK;
        }
        if (data_len == sizeof(s_fake.aht_frame)) {
            ++s_fake.aht_frame_reads;
            memcpy(data, s_fake.aht_frame, data_len);
            if (s_fake.aht_busy_reads > 0U) {
                data[0] |= 0x80U;
                --s_fake.aht_busy_reads;
            }
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)mem_address + data_len > sizeof(s_fake.registers[address])) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(data, &s_fake.registers[address][mem_address], data_len);
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
    if (!s_fake.responding[address]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (address == AHT_ADDRESS && mem_address == NULL_I2C_MEM_ADDR) {
        if (data_len == 3U && data[0] == 0xBEU) {
            s_fake.aht_status |= 0x08U;
        }
        return ESP_OK;
    }
    if ((size_t)mem_address + data_len > sizeof(s_fake.registers[address])) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_fake.registers[address][mem_address], data, data_len);
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks_to_wait)
{
    s_fake.last_delay_ticks = ticks_to_wait;
    s_fake.now_us += (int64_t)ticks_to_wait * INT64_C(10000) +
        s_fake.delay_extra_us;
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

static void test_bme280_skips_aht20_probe(void)
{
    reset_fake();
    setup_bmp280(0x76U, 0x60U);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);

    assert(barometer.bme280);
    assert(!barometer.reading.aht20_present);
    assert(!barometer.reading.aht20_valid);
    for (size_t i = 0U; i < s_fake.address_probe_count; ++i) {
        assert(s_fake.probed_addresses[i] != AHT_ADDRESS);
    }
    for (size_t i = 0U; i < s_fake.read_count; ++i) {
        assert(s_fake.read_addresses[i] != AHT_ADDRESS);
    }
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

static void test_aht20_crc_and_conversion(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    assert(barometer.reading.aht20_present);
    assert(barometer.reading.aht20_valid);
    assert(barometer.reading.humidity_mpct == 50000U);
    assert(barometer.reading.aht20_temperature_mc == 0);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_frame[6] ^= 1U;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_INVALID_STATE);
    assert(s_fake.aht_status == 0x08U);
    assert(barometer.reading.aht20_present);
    assert(!barometer.reading.aht20_valid);
    main_barometer_close(&barometer);
}

static void test_aht20_busy_timeout_and_transfer_error_classification(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_busy_reads = 100U;
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_TIMEOUT);
    assert(!barometer.reading.aht20_valid);
    main_barometer_close(&barometer);

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

static void test_aht20_monotonic_deadline_boundaries_and_minimum_tick(void)
{
    main_barometer_t barometer;

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_busy_reads = 8U;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_OK);
    assert(s_fake.now_us == INT64_C(90000));
    assert(s_fake.last_delay_ticks == 1U);
    assert(s_fake.aht_frame_reads == 9U);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_busy_reads = 9U;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_TIMEOUT);
    assert(s_fake.now_us == INT64_C(100000));
    assert(s_fake.aht_frame_reads == 9U);
    assert(!barometer.reading.aht20_valid);
    main_barometer_close(&barometer);

    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_busy_reads = 100U;
    s_fake.delay_extra_us = 1000U;
    main_barometer_init(&barometer);
    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_TIMEOUT);
    assert(s_fake.now_us == INT64_C(110000));
    assert(s_fake.last_delay_ticks == 1U);
    assert(s_fake.aht_frame_reads == 9U);
    assert(!barometer.reading.aht20_valid);
    main_barometer_close(&barometer);
}

static void test_absent_bmp280_retries_probe_only_for_five_seconds_then_reads_after_ack(void)
{
    reset_fake();
    s_fake.responding[AHT_ADDRESS] = false;
    main_barometer_t barometer;
    main_barometer_init(&barometer);

    assert(main_barometer_poll(&barometer, 0U) == ESP_ERR_NOT_FOUND);
    assert(s_fake.address_probe_count == 3U); /* BMP 0x77/0x76 and AHT20 */
    assert(s_fake.read_count == 0U);

    (void)main_barometer_poll(&barometer, 200U);
    (void)main_barometer_poll(&barometer, 4800U);
    assert(s_fake.address_probe_count == 3U);
    assert(s_fake.read_count == 0U);

    setup_bmp280(0x76U, BMP_ID);
    (void)main_barometer_poll(&barometer, 5000U);
    (void)main_barometer_poll(&barometer, 5200U);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.bmp280_address == 0x76U);
    assert(s_fake.address_probe_count > 3U);
    assert(s_fake.read_count > 0U);
    main_barometer_close(&barometer);
}

static void test_probe_timeout_skips_full_read_transaction(void)
{
    reset_fake();
    s_fake.responding[AHT_ADDRESS] = false;
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
    for (uint64_t now_ms = 0U; now_ms <= 10000U; now_ms += 1000U) {
        assert(main_barometer_poll(&barometer, now_ms) == ESP_OK);
        assert(s_fake.get_handles_calls == 0U);
        assert(s_fake.address_probe_count == 0U);
        assert(s_fake.read_count == 0U);
        assert(!barometer.reading.bmp280_present);
        assert(!barometer.reading.aht20_present);
    }
    main_barometer_request_polling();
    main_barometer_request_polling();
    assert(s_fake.get_handles_calls == 0U);
    assert(s_fake.address_probe_count == 0U);
    assert(s_fake.read_count == 0U);
    assert(main_barometer_poll(&barometer, 11000U) == ESP_OK);
    assert(s_fake.get_handles_calls == 1U);
    assert(barometer.reading.bmp280_present);
    assert(!barometer.reading.bmp280_valid);
    assert(barometer.reading.aht20_valid);
    assert(main_barometer_poll(&barometer, 12000U) == ESP_OK);
    assert(barometer.reading.bmp280_valid);
    assert(barometer.reading.pressure_pa > 0U);
    assert(barometer.reading.height_reference_state ==
        MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED);
    main_barometer_close(&barometer);
}

int main(void)
{
    test_boot_polling_does_not_touch_i2c_before_preflight();
    test_bmp280_compensation_datasheet_vector();
    test_bmp280_probes_preferred_then_fallback_address();
    test_bme280_accepted_as_bmp280();
    test_bme280_skips_aht20_probe();
    test_bmp280_rejects_wrong_chip_id();
    test_aht20_crc_and_conversion();
    test_aht20_busy_timeout_and_transfer_error_classification();
    test_aht20_monotonic_deadline_boundaries_and_minimum_tick();
    test_absent_bmp280_retries_probe_only_for_five_seconds_then_reads_after_ack();
    test_probe_timeout_skips_full_read_transaction();
    test_main_height_reference_stable_unstable_and_relative();
    test_main_height_reference_sensor_invalid_and_generation_validation();
    test_first_conversion_pending_is_not_a_bus_error();
    return 0;
}
