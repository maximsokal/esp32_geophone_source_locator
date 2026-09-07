#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_measurement_trace.h"


static void test_empty_input_clears_trace(void)
{
    int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    memset(trace, 0x7f, sizeof(trace));

    const float peak = ui_measurement_trace_reduce(NULL, 0U, trace);

    assert(peak == 0.0f);

    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS; ++point) {
            assert(trace[channel][point] == 0);
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

    int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(frames, 2U, trace);

    assert(peak == 22.6f);
    assert(trace[0][UI_MEASUREMENT_TRACE_POINTS - 2U] == 11);
    assert(trace[0][UI_MEASUREMENT_TRACE_POINTS - 1U] == -23);
    assert(trace[3][UI_MEASUREMENT_TRACE_POINTS - 2U] == 7);
    assert(trace[3][UI_MEASUREMENT_TRACE_POINTS - 1U] == 8);

    for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS - 2U; ++point) {
        assert(trace[0][point] == 0);
        assert(trace[3][point] == 0);
    }
}


static void test_bucket_keeps_signed_largest_impulse(void)
{
    seismic_frame_t frames[UI_MEASUREMENT_TRACE_POINTS * 2U] = {0};

    /* First chart bucket contains frames 0 and 1. */
    frames[0].channel_uv[0] = 100.0f;
    frames[1].channel_uv[0] = -950.0f;

    frames[0].channel_uv[1] = -1200.0f;
    frames[1].channel_uv[1] = 300.0f;

    /* A later single-frame impulse must survive compression as well. */
    frames[37].channel_uv[2] = 3210.0f;

    int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(
        frames,
        UI_MEASUREMENT_TRACE_POINTS * 2U,
        trace
    );

    assert(peak == 3210.0f);
    assert(trace[0][0] == -950);
    assert(trace[1][0] == -1200);

    /* 96 input frames / 48 buckets => frame 37 belongs to bucket 18. */
    assert(trace[2][18] == 3210);
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

    int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    const float peak = ui_measurement_trace_reduce(
        frames,
        UI_MEASUREMENT_TRACE_POINTS * 2U,
        trace
    );

    assert(peak == 80.0f);
    assert(trace[0][0] == 20);
    assert(trace[1][0] == 40);
    assert(trace[2][0] == -60);
    assert(trace[3][0] == 80);
}


int main(void)
{
    test_empty_input_clears_trace();
    test_short_window_is_right_aligned();
    test_bucket_keeps_signed_largest_impulse();
    test_channels_are_reduced_independently();
    return 0;
}
