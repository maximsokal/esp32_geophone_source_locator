#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "nrf_ema_filter.h"
#include "sensor_bus.h"

typedef struct {
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint16_t dig_p1;
    int16_t dig_p2;
    int16_t dig_p3;
    int16_t dig_p4;
    int16_t dig_p5;
    int16_t dig_p6;
    int16_t dig_p7;
    int16_t dig_p8;
    int16_t dig_p9;
} main_barometer_bmp280_calibration_t;

typedef enum {
    MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED = 0,
    MAIN_BAROMETER_REFERENCE_VALID,
    MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE,
    MAIN_BAROMETER_REFERENCE_SENSOR_INVALID,
} main_barometer_reference_state_t;

typedef struct {
    bool bmp280_present;
    bool bmp280_valid;
    uint8_t bmp280_address;
    uint32_t pressure_pa;
    int32_t temperature_mc;
    double filtered_pressure_pa;
    int32_t filtered_altitude_mm;
    main_barometer_reference_state_t height_reference_state;
    uint16_t height_reference_generation;
    int32_t relative_z_mm;
    bool aht20_present;
    bool aht20_valid;
    uint32_t humidity_mpct;
    int32_t aht20_temperature_mc;
} main_barometer_reading_t;

typedef struct {
    sensor_bus_handles_t handles;
    i2c_bus_device_handle_t bmp280;
    i2c_bus_device_handle_t aht20;
    main_barometer_bmp280_calibration_t calibration;
    nrf_ema_filter_t altitude_filter;
    nrf_ema_filter_t pressure_filter;
    main_barometer_reading_t reading;
    uint64_t next_bmp280_poll_ms;
    uint64_t bmp280_retry_after_ms;
    uint64_t aht20_retry_after_ms;
    uint64_t last_aht20_attempt_ms;
    esp_err_t bmp280_retry_error;
    esp_err_t aht20_retry_error;
    bool bmp280_initialized;
    bool aht20_initialized;
    bool bmp280_probe_required;
    bool bme280;
    bool aht20_probe_required;
    bool aht20_attempted;
    bool height_reference_capture_active;
    bool height_reference_has_sample;
    uint64_t height_reference_capture_started_ms;
    double height_reference_min_pressure_pa;
    double height_reference_max_pressure_pa;
    int32_t height_reference_altitude_mm;
} main_barometer_t;

void main_barometer_init(main_barometer_t *barometer);
esp_err_t main_barometer_request_height_reference_capture(uint16_t generation);
esp_err_t main_barometer_poll(main_barometer_t *barometer, uint64_t now_ms);
void main_barometer_close(main_barometer_t *barometer);
void main_barometer_get_reading(
    const main_barometer_t *barometer,
    main_barometer_reading_t *reading);

bool main_barometer_bmp280_compensate(
    uint32_t raw_pressure,
    uint32_t raw_temperature,
    const main_barometer_bmp280_calibration_t *calibration,
    uint32_t *pressure_pa,
    int32_t *temperature_mc);

uint8_t main_barometer_aht20_crc8(const uint8_t *data, uint8_t length);
