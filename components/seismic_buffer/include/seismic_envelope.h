#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "seismic_buffer.h"


typedef struct
{
    size_t frame_count;
    size_t frames_consumed;
    size_t bucket_count;
    size_t bucket_capacity;
    double channel_sum[SEISMIC_CHANNEL_COUNT];
    float *bucket_min;
    float *bucket_max;
    bool *bucket_initialized;
    float latest[SEISMIC_CHANNEL_COUNT];

} seismic_envelope_accumulator_t;


bool seismic_envelope_accumulator_begin(
    seismic_envelope_accumulator_t *accumulator,
    size_t frame_count,
    size_t bucket_count,
    size_t bucket_capacity,
    float *bucket_min,
    float *bucket_max,
    bool *bucket_initialized
);

/* Caller storage is channel-major with bucket_capacity stride; chunks are sequential. */
bool seismic_envelope_accumulator_add(
    seismic_envelope_accumulator_t *accumulator,
    size_t frame_offset,
    const seismic_frame_t *frames,
    size_t frame_count
);

/* Optionally subtract each channel's mean in place after all frames arrive. */
bool seismic_envelope_accumulator_finish(
    seismic_envelope_accumulator_t *accumulator,
    bool subtract_mean
);
