#include "seismic_envelope.h"

#include <string.h>


bool seismic_envelope_accumulator_begin(
    seismic_envelope_accumulator_t *accumulator,
    size_t frame_count,
    size_t bucket_count,
    size_t bucket_capacity,
    float *bucket_min,
    float *bucket_max,
    bool *bucket_initialized
)
{
    if (
        accumulator == NULL || frame_count == 0U || bucket_count == 0U ||
        bucket_count > frame_count || bucket_count > bucket_capacity ||
        bucket_capacity == 0U || bucket_capacity > SIZE_MAX / sizeof(bool) ||
        bucket_min == NULL || bucket_max == NULL || bucket_initialized == NULL
    ) {
        return false;
    }

    memset(accumulator, 0, sizeof(*accumulator));
    memset(bucket_initialized, 0, bucket_capacity * sizeof(*bucket_initialized));
    accumulator->frame_count = frame_count;
    accumulator->bucket_count = bucket_count;
    accumulator->bucket_capacity = bucket_capacity;
    accumulator->bucket_min = bucket_min;
    accumulator->bucket_max = bucket_max;
    accumulator->bucket_initialized = bucket_initialized;
    return true;
}


bool seismic_envelope_accumulator_add(
    seismic_envelope_accumulator_t *accumulator,
    size_t frame_offset,
    const seismic_frame_t *frames,
    size_t frame_count
)
{
    if (
        accumulator == NULL || frames == NULL || frame_count == 0U ||
        frame_offset != accumulator->frames_consumed ||
        frame_offset > accumulator->frame_count ||
        frame_count > accumulator->frame_count - frame_offset
    ) {
        return false;
    }

    for (size_t i = 0U; i < frame_count; ++i) {
        const size_t frame_index = frame_offset + i;
        const size_t bucket = (size_t)(
            (uint64_t)frame_index * accumulator->bucket_count /
            accumulator->frame_count
        );
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const float value = frames[i].channel_uv[channel];
            const size_t index = channel * accumulator->bucket_capacity + bucket;
            accumulator->channel_sum[channel] += (double)value;
            accumulator->latest[channel] = value;
            if (!accumulator->bucket_initialized[bucket]) {
                accumulator->bucket_min[index] = value;
                accumulator->bucket_max[index] = value;
            }
            else {
                if (value < accumulator->bucket_min[index]) {
                    accumulator->bucket_min[index] = value;
                }
                if (value > accumulator->bucket_max[index]) {
                    accumulator->bucket_max[index] = value;
                }
            }
        }
        accumulator->bucket_initialized[bucket] = true;
    }
    accumulator->frames_consumed += frame_count;
    return true;
}


bool seismic_envelope_accumulator_finish(
    seismic_envelope_accumulator_t *accumulator,
    bool subtract_mean
)
{
    if (
        accumulator == NULL || accumulator->frame_count == 0U ||
        accumulator->frames_consumed != accumulator->frame_count ||
        accumulator->bucket_min == NULL || accumulator->bucket_max == NULL ||
        accumulator->bucket_initialized == NULL
    ) {
        return false;
    }

    for (size_t bucket = 0U; bucket < accumulator->bucket_count; ++bucket) {
        if (!accumulator->bucket_initialized[bucket]) return false;
    }

    if (subtract_mean) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const float mean = (float)(
                accumulator->channel_sum[channel] / (double)accumulator->frame_count
            );
            for (size_t bucket = 0U; bucket < accumulator->bucket_count; ++bucket) {
                const size_t index = channel * accumulator->bucket_capacity + bucket;
                accumulator->bucket_min[index] -= mean;
                accumulator->bucket_max[index] -= mean;
            }
        }
    }
    return true;
}
