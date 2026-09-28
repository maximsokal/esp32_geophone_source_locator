#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_measurement_trace.h"
#include "ui_measurements_internal.h"


static void test_empty_input_clears_trace(void)
{
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    memset(trace_min, 0x7f, sizeof(trace_min));
    memset(trace_max, 0x7f, sizeof(trace_max));

    const float peak = ui_measurement_trace_reduce(
        NULL,
        0U,
        trace_min,
        trace_max
    );

    assert(peak == 0.0f);

    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS; ++point) {
            assert(trace_min[channel][point] == 0);
            assert(trace_max[channel][point] == 0);
        }
    }
}


static void test_short_window_is_right_aligned(void)
{
    seismic_frame_t frames[2] = {0};
    frames[0].channel_uv[0] = 11.4f;
    frames[1].channel_uv[0] = -22.6f;
    frames[0].channel_uv[3] = 7.0f;
    frames[1].channel_uv[3] = 8.0f;

    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(
        frames,
        2U,
        trace_min,
        trace_max
    );

    assert(peak == 17.0f);
    assert(trace_min[0][UI_MEASUREMENT_TRACE_POINTS - 2U] == 17);
    assert(trace_max[0][UI_MEASUREMENT_TRACE_POINTS - 2U] == 17);
    assert(trace_min[0][UI_MEASUREMENT_TRACE_POINTS - 1U] == -17);
    assert(trace_max[0][UI_MEASUREMENT_TRACE_POINTS - 1U] == -17);
    assert(trace_min[3][UI_MEASUREMENT_TRACE_POINTS - 2U] == -1);
    assert(trace_max[3][UI_MEASUREMENT_TRACE_POINTS - 2U] == -1);
    assert(trace_min[3][UI_MEASUREMENT_TRACE_POINTS - 1U] == 1);
    assert(trace_max[3][UI_MEASUREMENT_TRACE_POINTS - 1U] == 1);

    for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS - 2U; ++point) {
        assert(trace_min[0][point] == 0);
        assert(trace_max[0][point] == 0);
        assert(trace_min[3][point] == 0);
        assert(trace_max[3][point] == 0);
    }
}


static void test_bucket_keeps_min_and_max_envelope(void)
{
    seismic_frame_t frames[UI_MEASUREMENT_TRACE_POINTS * 2U] = {0};

    /* First chart bucket contains frames 0 and 1. */
    frames[0].channel_uv[0] = 100.0f;
    frames[1].channel_uv[0] = -950.0f;

    frames[0].channel_uv[1] = -1200.0f;
    frames[1].channel_uv[1] = 300.0f;

    /* A later single-frame impulse must survive compression as well. */
    frames[37].channel_uv[2] = 3210.0f;

    seismic_frame_t original[UI_MEASUREMENT_TRACE_POINTS * 2U];
    memcpy(original, frames, sizeof(original));

    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(
        frames,
        UI_MEASUREMENT_TRACE_POINTS * 2U,
        trace_min,
        trace_max
    );

    /* The bucket retains both sides of the waveform, not only -950. */
    assert(trace_min[0][0] == -941);
    assert(trace_max[0][0] == 109);
    assert(trace_min[1][0] == -1191);
    assert(trace_max[1][0] == 309);

    /* 96 input frames / 48 buckets => frame 37 belongs to bucket 18. */
    assert(trace_min[2][18] == -33);
    assert(trace_max[2][18] == 3177);
    assert(peak > 3176.0f && peak < 3177.0f);
    assert(memcmp(frames, original, sizeof(frames)) == 0);
}


static void test_channels_are_reduced_independently(void)
{
    seismic_frame_t frames[UI_MEASUREMENT_TRACE_POINTS * 2U] = {0};

    frames[0].channel_uv[0] = 10.0f;
    frames[1].channel_uv[0] = 20.0f;

    frames[0].channel_uv[1] = 40.0f;
    frames[1].channel_uv[1] = -30.0f;

    frames[0].channel_uv[2] = -60.0f;
    frames[1].channel_uv[2] = 50.0f;

    frames[0].channel_uv[3] = -70.0f;
    frames[1].channel_uv[3] = 80.0f;

    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(
        frames,
        2U,
        trace_min,
        trace_max
    );

    assert(peak == 75.0f);
    assert(trace_min[0][UI_MEASUREMENT_TRACE_POINTS - 2U] == -5);
    assert(trace_max[0][UI_MEASUREMENT_TRACE_POINTS - 2U] == -5);
    assert(trace_min[0][UI_MEASUREMENT_TRACE_POINTS - 1U] == 5);
    assert(trace_max[0][UI_MEASUREMENT_TRACE_POINTS - 1U] == 5);
    assert(trace_min[1][UI_MEASUREMENT_TRACE_POINTS - 2U] == 35);
    assert(trace_max[1][UI_MEASUREMENT_TRACE_POINTS - 1U] == -35);
    assert(trace_min[2][UI_MEASUREMENT_TRACE_POINTS - 2U] == -55);
    assert(trace_max[2][UI_MEASUREMENT_TRACE_POINTS - 1U] == 55);
    assert(trace_min[3][UI_MEASUREMENT_TRACE_POINTS - 2U] == -75);
    assert(trace_max[3][UI_MEASUREMENT_TRACE_POINTS - 1U] == 75);
}


static void test_preview_span_is_time_based(void)
{
    assert(ui_measurements_preview_frames_for_rate(0U) == 0U);
    assert(ui_measurements_preview_frames_for_rate(500U) == 512U);
    assert(ui_measurements_preview_frames_for_rate(20000U) == 20480U);
    assert(ui_measurements_preview_frames_for_rate(40000U) == 40960U);
}


int main(void)
{
    test_empty_input_clears_trace();
    test_short_window_is_right_aligned();
    test_bucket_keeps_min_and_max_envelope();
    test_channels_are_reduced_independently();
    test_preview_span_is_time_based();
    return 0;
}
