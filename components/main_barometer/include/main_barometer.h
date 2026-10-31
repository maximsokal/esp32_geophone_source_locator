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
    /* Times the sensor was found back in its reset state (CTRL_MEAS/CONFIG lost) and re-armed; saturating. */
    uint16_t config_lost_count;
    /* Diagnostics for a failed MAIN calibration row (all saturating): sensor (re)probes that succeeded,
     * bus-level failures that sent the driver back to probing, and the poll time of the last sample. */
    uint16_t bmp280_probe_count;
    uint16_t bmp280_bus_error_count;
    uint32_t last_sample_ms; /* 0 = no sample yet */
} main_barometer_reading_t;

typedef enum {
    MAIN_BMP280_PROBE = 0,
    MAIN_BMP280_CONFIGURE,
    MAIN_BMP280_VERIFY,
    MAIN_BMP280_MEASURING,
    MAIN_BMP280_RECOVER,
} main_bmp280_state_t;

typedef struct {
    sensor_bus_handles_t handles;
    i2c_bus_device_handle_t bmp280;
    main_barometer_bmp280_calibration_t calibration;
    nrf_ema_filter_t altitude_filter;
    nrf_ema_filter_t pressure_filter;
    main_barometer_reading_t reading;
    uint64_t next_bmp280_poll_ms;
    uint64_t bmp280_retry_after_ms;
    uint64_t bmp280_pending_count_due_ms;
    esp_err_t bmp280_retry_error;
    /* Consecutive polls without a proven new sample. */
    uint8_t bmp280_pending_polls;
    /* Consecutive polls that found the sensor in its reset state; cleared by a fresh sample. */
    uint8_t bmp280_config_lost_polls;
    /* Set by one read when it re-armed a reset sensor: that poll is not a slow conversion. */
    bool bmp280_config_recovered;
    /* Consecutive polls that lost the shared-bus owner lock to another task. */
    uint8_t bmp280_yield_polls;
    /* Normal mode stalled: start and finish forced conversions across polls. */
    bool bmp280_forced;
    bool bmp280_conversion_pending;
    bool bmp280_conversion_seen;
    uint64_t bmp280_conversion_ready_ms;
    uint64_t bmp280_conversion_deadline_ms;
    main_bmp280_state_t bmp280_state;
    bool bmp280_initialized;
    bool bme280;
    bool height_reference_capture_active;
    bool height_reference_has_sample;
    uint64_t height_reference_capture_started_ms;
    /* Last time the active capture made progress: its start, or a sample that fed the window. */
    uint64_t height_reference_progress_ms;
    /* Time of the poll in progress, for the log line raised inside the capture bookkeeping. */
    uint64_t last_poll_ms;
    /* Last poll that produced a fresh sample while the reference was valid. */
    uint64_t height_reference_last_good_ms;
    /* Held reference timed out; a fresh sample restores it. */
    bool height_reference_restorable;
    double height_reference_min_pressure_pa;
    double height_reference_max_pressure_pa;
    int32_t height_reference_altitude_mm;
} main_barometer_t;

void main_barometer_init(main_barometer_t *barometer);
/* Thread-safe, idempotent request; the existing worker polls from the next
 * cycle onward. Until requested, poll performs no I2C. */
void main_barometer_request_polling(void);
esp_err_t main_barometer_request_height_reference_capture(uint16_t generation);
esp_err_t main_barometer_poll(main_barometer_t *barometer, uint64_t now_ms);
/* How long the polling task may sleep before the next poll is due, capped at
 * max_ms. Returns max_ms while polling is not requested or the bus is not open. */
uint32_t main_barometer_ms_until_poll(
    const main_barometer_t *barometer,
    uint64_t now_ms,
    uint32_t max_ms);
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
