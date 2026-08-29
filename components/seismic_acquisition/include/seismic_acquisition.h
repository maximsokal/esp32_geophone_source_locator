#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"


#define SEISMIC_ACQUISITION_MAX_CHANNELS  8


typedef struct
{
    bool running;
    bool latest_valid;

    const char *source_name;
    const char *transport_name;

    uint32_t sample_rate_hz;
    uint32_t bus_clock_hz;

    uint8_t channel_count;
    uint8_t bus_mode;
    uint8_t oversampling_ratio;

    int core_id;

    float input_range_v;

    uint64_t samples;
    uint64_t read_errors;
    uint32_t consecutive_failures;

    int64_t last_sample_timestamp_us;

    float latest_uv[
        SEISMIC_ACQUISITION_MAX_CHANNELS
    ];

} seismic_acquisition_snapshot_t;


esp_err_t seismic_acquisition_start(void);

void seismic_acquisition_stop(void);

bool seismic_acquisition_is_running(void);

const char *seismic_acquisition_source_name(void);

esp_err_t seismic_acquisition_get_snapshot(
    seismic_acquisition_snapshot_t *snapshot
);
