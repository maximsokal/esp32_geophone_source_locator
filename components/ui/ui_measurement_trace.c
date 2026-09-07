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


float ui_measurement_trace_reduce(
    const seismic_frame_t *frames,
    size_t frame_count,
    int32_t trace[
        SEISMIC_CHANNEL_COUNT
    ][
        UI_MEASUREMENT_TRACE_POINTS
    ]
)
{
    if (trace == NULL) {
        return 0.0f;
    }

    ui_measurement_trace_clear(trace);

    if (frames == NULL || frame_count == 0U) {
        return 0.0f;
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
            float selected = frames[start].channel_uv[channel];
            float selected_abs = ui_measurement_trace_absf(selected);

            for (size_t index = start + 1U; index < end; ++index) {
                const float candidate = frames[index].channel_uv[channel];
                const float candidate_abs = ui_measurement_trace_absf(candidate);

                if (candidate_abs > selected_abs) {
                    selected = candidate;
                    selected_abs = candidate_abs;
                }
            }

            trace[channel][output_offset + bucket] =
                ui_measurement_trace_round_to_i32(selected);

            if (selected_abs > max_abs_uv) {
                max_abs_uv = selected_abs;
            }
        }
    }

    return max_abs_uv;
}
