#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED = 0,
    MAIN_BAROMETER_REFERENCE_VALID,
    MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE,
    MAIN_BAROMETER_REFERENCE_SENSOR_INVALID,
} main_barometer_reference_state_t;

typedef struct {
    bool bmp280_present;
    bool bmp280_valid;
    uint32_t pressure_pa;
    int32_t temperature_mc;
    main_barometer_reference_state_t height_reference_state;
    uint16_t height_reference_generation;
    int32_t relative_z_mm;
    bool aht20_present;
    bool aht20_valid;
    uint32_t humidity_mpct;
} main_barometer_reading_t;

typedef struct {
    uint8_t unused;
} main_barometer_t;

void main_barometer_init(main_barometer_t *barometer);
esp_err_t main_barometer_poll(main_barometer_t *barometer, uint64_t now_ms);
void main_barometer_close(main_barometer_t *barometer);
void main_barometer_get_reading(
    const main_barometer_t *barometer,
    main_barometer_reading_t *reading
);
