#include "ui_measurement_trace.h"

#include <string.h>


static float ui_measurement_trace_absf(float value)
{
    return value < 0.0f ? -value : value;
}


static int32_t ui_measurement_trace_round_to_i32(float value)
{
    return value >= 0.0f
        ? (int32_t)(value + 0.5f)
        : (int32_t)(value - 0.5f);
}


static void ui_measurement_trace_clear(
    int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS]
)
{
    if (trace != NULL) {
        memset(trace, 0, sizeof(int32_t) * SEISMIC_CHANNEL_COUNT * UI_MEASUREMENT_TRACE_POINTS);
    }
}


bool ui_measurement_trace_accumulator_begin(
    ui_measurement_trace_accumulator_t *accumulator,
    size_t frame_count
)
{
    if (accumulator == NULL || frame_count == 0U) {
        return false;
    }

    memset(accumulator, 0, sizeof(*accumulator));
    const size_t bucket_count = frame_count < UI_MEASUREMENT_TRACE_POINTS
        ? frame_count
        : UI_MEASUREMENT_TRACE_POINTS;
    return seismic_envelope_accumulator_begin(
        &accumulator->envelope,
        frame_count,
        bucket_count,
        UI_MEASUREMENT_TRACE_POINTS,
        accumulator->bucket_min,
        accumulator->bucket_max,
        accumulator->bucket_initialized
    );
}


bool ui_measurement_trace_accumulator_add(
    ui_measurement_trace_accumulator_t *accumulator,
    size_t frame_offset,
    const seismic_frame_t *frames,
    size_t frame_count
)
{
    return accumulator != NULL && seismic_envelope_accumulator_add(
        &accumulator->envelope, frame_offset, frames, frame_count
    );
}


float ui_measurement_trace_accumulator_finish(
    ui_measurement_trace_accumulator_t *accumulator,
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    float latest[SEISMIC_CHANNEL_COUNT],
    float channel_min[SEISMIC_CHANNEL_COUNT],
    float channel_max[SEISMIC_CHANNEL_COUNT]
)
{
    ui_measurement_trace_clear(trace_min);
    ui_measurement_trace_clear(trace_max);
    if (
        accumulator == NULL || trace_min == NULL || trace_max == NULL ||
        !seismic_envelope_accumulator_finish(&accumulator->envelope, true)
    ) {
        return 0.0f;
    }

    const size_t output_offset =
        UI_MEASUREMENT_TRACE_POINTS - accumulator->envelope.bucket_count;
    float max_abs_uv = 0.0f;
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        float overall_min = 0.0f;
        float overall_max = 0.0f;
        for (size_t bucket = 0U; bucket < accumulator->envelope.bucket_count; ++bucket) {
            const size_t index = channel * UI_MEASUREMENT_TRACE_POINTS + bucket;
            const float minimum = accumulator->bucket_min[index];
            const float maximum = accumulator->bucket_max[index];
            const size_t output = output_offset + bucket;
            trace_min[channel][output] = ui_measurement_trace_round_to_i32(minimum);
            trace_max[channel][output] = ui_measurement_trace_round_to_i32(maximum);

            if (bucket == 0U || minimum < overall_min) overall_min = minimum;
            if (bucket == 0U || maximum > overall_max) overall_max = maximum;
            const float minimum_abs = ui_measurement_trace_absf(minimum);
            const float maximum_abs = ui_measurement_trace_absf(maximum);
            if (minimum_abs > max_abs_uv) max_abs_uv = minimum_abs;
            if (maximum_abs > max_abs_uv) max_abs_uv = maximum_abs;
        }
        if (latest != NULL) latest[channel] = accumulator->envelope.latest[channel];
        if (channel_min != NULL) channel_min[channel] = overall_min;
        if (channel_max != NULL) channel_max[channel] = overall_max;
    }
    return max_abs_uv;
}


float ui_measurement_trace_reduce(
    const seismic_frame_t *frames,
    size_t frame_count,
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS]
)
{
    if (trace_min == NULL || trace_max == NULL) {
        return 0.0f;
    }
    if (frames == NULL || frame_count == 0U) {
        ui_measurement_trace_clear(trace_min);
        ui_measurement_trace_clear(trace_max);
        return 0.0f;
    }

    ui_measurement_trace_accumulator_t accumulator;
    if (
        !ui_measurement_trace_accumulator_begin(&accumulator, frame_count) ||
        !ui_measurement_trace_accumulator_add(&accumulator, 0U, frames, frame_count)
    ) {
        ui_measurement_trace_clear(trace_min);
        ui_measurement_trace_clear(trace_max);
        return 0.0f;
    }
    return ui_measurement_trace_accumulator_finish(
        &accumulator, trace_min, trace_max, NULL, NULL, NULL
    );
}
