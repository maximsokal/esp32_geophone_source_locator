#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seismic_buffer.h"


#define UI_MEASUREMENT_TRACE_POINTS  48U

typedef struct
{
    size_t frame_count;
    size_t frames_consumed;
    size_t bucket_count;
    double channel_sum[SEISMIC_CHANNEL_COUNT];
    float bucket_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    float bucket_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    bool bucket_initialized[UI_MEASUREMENT_TRACE_POINTS];
    float latest[SEISMIC_CHANNEL_COUNT];

} ui_measurement_trace_accumulator_t;


bool ui_measurement_trace_accumulator_begin(
    ui_measurement_trace_accumulator_t *accumulator,
    size_t frame_count
);

bool ui_measurement_trace_accumulator_add(
    ui_measurement_trace_accumulator_t *accumulator,
    size_t frame_offset,
    const seismic_frame_t *frames,
    size_t frame_count
);

float ui_measurement_trace_accumulator_finish(
    ui_measurement_trace_accumulator_t *accumulator,
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    float latest[SEISMIC_CHANNEL_COUNT],
    float channel_min[SEISMIC_CHANNEL_COUNT],
    float channel_max[SEISMIC_CHANNEL_COUNT]
);


/*
 * Compress a recent seismic frame window into a display-only min/max
 * envelope. The mean of each channel is removed from the local display copy
 * before bucketing; the input frames are never modified.
 *
 * When fewer frames than display points are available, samples are right
 * aligned so the newest data remains at the right edge of the chart.
 * Returns the greatest absolute display-envelope magnitude in microvolts.
 */
float ui_measurement_trace_reduce(
    const seismic_frame_t *frames,
    size_t frame_count,
    int32_t trace_min[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ],
    int32_t trace_max[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ]
);
