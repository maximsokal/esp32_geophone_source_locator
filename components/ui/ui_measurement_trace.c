#include "ui_measurement_trace.h"


static float ui_measurement_trace_absf(float value)
{
    return value < 0.0f ? -value : value;
}


static int32_t ui_measurement_trace_round_to_i32(float value)
{
    if (value >= 0.0f) {
        return (int32_t)(value + 0.5f);
    }

    return (int32_t)(value - 0.5f);
}


static void ui_measurement_trace_clear(
    int32_t trace[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ]
)
{
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS; ++point) {
            trace[channel][point] = 0;
        }
    }
}


static void ui_measurement_trace_clear_pair(
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
)
{
    ui_measurement_trace_clear(trace_min);
    ui_measurement_trace_clear(trace_max);
}


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
)
{
    if (trace_min == NULL || trace_max == NULL) {
        return 0.0f;
    }

    ui_measurement_trace_clear_pair(trace_min, trace_max);

    if (frames == NULL || frame_count == 0U) {
        return 0.0f;
    }

    double channel_sum[SEISMIC_CHANNEL_COUNT] = {0.0};
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            channel_sum[channel] +=
                (double)frames[frame].channel_uv[channel];
        }
    }

    float channel_mean[SEISMIC_CHANNEL_COUNT] = {0.0f};
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        channel_mean[channel] =
            (float)(channel_sum[channel] / (double)frame_count);
    }

    const size_t bucket_count =
        frame_count < UI_MEASUREMENT_TRACE_POINTS
            ? frame_count
            : UI_MEASUREMENT_TRACE_POINTS;

    const size_t output_offset =
        UI_MEASUREMENT_TRACE_POINTS - bucket_count;

    float max_abs_uv = 0.0f;

    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
        size_t start =
            (bucket * frame_count) /
            bucket_count;

        size_t end =
            ((bucket + 1U) * frame_count) /
            bucket_count;

        if (end <= start) {
            end = start + 1U;
        }

        if (end > frame_count) {
            end = frame_count;
        }

        for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            float minimum =
                frames[start].channel_uv[channel] - channel_mean[channel];
            float maximum = minimum;

            for (size_t index = start + 1U; index < end; ++index) {
                const float candidate =
                    frames[index].channel_uv[channel] - channel_mean[channel];

                if (candidate < minimum) minimum = candidate;
                if (candidate > maximum) maximum = candidate;
            }

            const int32_t minimum_i32 =
                ui_measurement_trace_round_to_i32(minimum);
            const int32_t maximum_i32 =
                ui_measurement_trace_round_to_i32(maximum);

            trace_min[channel][output_offset + bucket] = minimum_i32;
            trace_max[channel][output_offset + bucket] = maximum_i32;

            const float minimum_abs = ui_measurement_trace_absf(minimum);
            const float maximum_abs = ui_measurement_trace_absf(maximum);
            if (minimum_abs > max_abs_uv) max_abs_uv = minimum_abs;
            if (maximum_abs > max_abs_uv) max_abs_uv = maximum_abs;
        }
    }

    return max_abs_uv;
}
