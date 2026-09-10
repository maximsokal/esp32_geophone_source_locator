/*
 * Host contract for the live Measurements pipeline.
 *
 * This deliberately exercises the production seismic_buffer and display
 * reducer boundaries with three independent consumer cursors.  It does not
 * claim to emulate the AD7606 SPI DMA engine, FAT/SD, TFT, Wi-Fi or GPIO
 * hardware; those remain target/HIL responsibilities.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "app_task_config.h"
#include "seismic_buffer.h"
#include "ui_measurement_trace.h"


#define PIPELINE_SAMPLE_RATE_HZ  500U
#define PIPELINE_INITIAL_FRAMES  96U
#define PIPELINE_IMPULSE_FRAME   65U


static void pipeline_channels(
    uint64_t frame_number,
    float channels[SEISMIC_CHANNEL_COUNT]
)
{
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        channels[channel] =
            (float)(frame_number * 10U + (uint64_t)channel);
    }

    if (frame_number == PIPELINE_IMPULSE_FRAME) {
        channels[0] = 18000.0f;
        channels[1] = -12000.0f;
    }
}


static void assert_pipeline_frame(
    const seismic_frame_t *frame,
    uint64_t frame_number
)
{
    float expected[SEISMIC_CHANNEL_COUNT];
    pipeline_channels(frame_number, expected);

    assert(frame->timestamp_us == (int64_t)frame_number * 2000LL);
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        assert(frame->channel_uv[channel] == expected[channel]);
    }
}


static bool trace_contains(
    const int32_t trace[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS],
    size_t channel,
    int32_t expected
)
{
    for (size_t point = 0; point < UI_MEASUREMENT_TRACE_POINTS; ++point) {
        if (trace[channel][point] == expected) {
            return true;
        }
    }

    return false;
}


static void test_pipeline_fan_out_preserves_signal(void)
{
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(
        SEISMIC_SOURCE_AD7606,
        PIPELINE_SAMPLE_RATE_HZ
    ) == ESP_OK);

    uint64_t recorder_cursor = 0U;
    uint64_t ui_cursor = 0U;
    uint64_t recorder_samples = 0U;
    float channels[SEISMIC_CHANNEL_COUNT];

    /* Acquisition continues while each consumer is serviced independently. */
    for (uint64_t frame_number = 1U;
         frame_number <= PIPELINE_INITIAL_FRAMES;
         ++frame_number) {
        pipeline_channels(frame_number, channels);
        assert(seismic_buffer_push(
            (int64_t)frame_number * 2000LL,
            channels
        ) == ESP_OK);

        /* Recorder gets bounded batches and therefore never copies the ring
         * while holding the producer metadata critical section. */
        if ((frame_number % 7U) == 0U || frame_number == PIPELINE_INITIAL_FRAMES) {
            seismic_frame_t recorder_batch[7];
            size_t count = 0U;
            uint64_t dropped = 0U;
            seismic_buffer_info_t info = { 0 };
            assert(seismic_buffer_copy_since(
                &recorder_cursor,
                recorder_batch,
                7U,
                &count,
                &dropped,
                &info
            ) == ESP_OK);
            assert(dropped == 0U);
            assert(count <= 7U);
            assert(info.source == SEISMIC_SOURCE_AD7606);
            assert(info.sample_rate_hz == PIPELINE_SAMPLE_RATE_HZ);
            for (size_t i = 0; i < count; ++i) {
                assert_pipeline_frame(
                    &recorder_batch[i],
                    recorder_samples + (uint64_t)i + 1U
                );
            }
            recorder_samples += (uint64_t)count;
        }

        /* UI and web have independent cursors.  They can be delayed or
         * scheduled at different rates without consuming recorder frames. */
        if ((frame_number % 3U) == 0U) {
            seismic_frame_t ui_batch[5];
            size_t count = 0U;
            uint64_t dropped = 0U;
            seismic_buffer_info_t info = { 0 };
            assert(seismic_buffer_copy_since(
                &ui_cursor,
                ui_batch,
                5U,
                &count,
                &dropped,
                &info
            ) == ESP_OK);
            assert(dropped == 0U);
            assert(count <= 5U);
            if (count > 0U) {
                assert(ui_batch[count - 1U].timestamp_us <=
                       (int64_t)frame_number * 2000LL);
            }
        }

        if ((frame_number % 11U) == 0U) {
            seismic_frame_t web_snapshot[16];
            size_t count = 0U;
            seismic_buffer_info_t info = { 0 };
            assert(seismic_buffer_copy_latest(
                web_snapshot,
                16U,
                &count,
                &info
            ) == ESP_OK);
            assert(count <= 16U);
            assert(info.source == SEISMIC_SOURCE_AD7606);
            assert(info.sample_rate_hz == PIPELINE_SAMPLE_RATE_HZ);
            if (count > 0U) {
                assert(web_snapshot[count - 1U].timestamp_us ==
                       (int64_t)frame_number * 2000LL);
            }
            if (frame_number == 66U) {
                bool impulse_seen = false;
                for (size_t i = 0; i < count; ++i) {
                    if (web_snapshot[i].channel_uv[0] == 18000.0f &&
                        web_snapshot[i].channel_uv[1] == -12000.0f) {
                        impulse_seen = true;
                        break;
                    }
                }
                assert(impulse_seen);
            }
        }
    }

    assert(recorder_samples == PIPELINE_INITIAL_FRAMES);
    assert(recorder_cursor == PIPELINE_INITIAL_FRAMES);

    /* The UI's display reducer keeps the min/max envelope, so a short impulse
     * cannot disappear merely because 96 samples are compressed to 48 points. */
    seismic_frame_t preview[PIPELINE_INITIAL_FRAMES];
    size_t preview_count = 0U;
    seismic_buffer_info_t preview_info = { 0 };
    assert(seismic_buffer_copy_latest(
        preview,
        PIPELINE_INITIAL_FRAMES,
        &preview_count,
        &preview_info
    ) == ESP_OK);
    assert(preview_count == PIPELINE_INITIAL_FRAMES);
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    assert(ui_measurement_trace_reduce(
        preview,
        preview_count,
        trace_min,
        trace_max
    ) > 17000.0f);
    assert(trace_contains(trace_max, 0U, 17334));
    assert(trace_contains(trace_min, 1U, -12354));

    /* A delayed recorder loses only the bounded oldest portion; the producer
     * remains publishable and UI/web snapshots still expose the newest data. */
    assert(seismic_buffer_configure(
        SEISMIC_SOURCE_AD7606,
        PIPELINE_SAMPLE_RATE_HZ
    ) == ESP_OK);
    const uint64_t delayed_total =
        (uint64_t)SEISMIC_BUFFER_CAPACITY + 9U;
    for (uint64_t frame_number = 1U;
         frame_number <= delayed_total;
         ++frame_number) {
        pipeline_channels(frame_number, channels);
        assert(seismic_buffer_push(
            (int64_t)frame_number * 2000LL,
            channels
        ) == ESP_OK);
    }

    seismic_frame_t delayed_batch[8];
    size_t delayed_count = 0U;
    uint64_t delayed_dropped = 0U;
    seismic_buffer_info_t delayed_info = { 0 };
    uint64_t delayed_cursor = 0U;
    assert(seismic_buffer_copy_since(
        &delayed_cursor,
        delayed_batch,
        8U,
        &delayed_count,
        &delayed_dropped,
        &delayed_info
    ) == ESP_OK);
    assert(delayed_dropped == 9U);
    assert(delayed_count == 8U);
    assert(delayed_cursor == 17U);
    assert_pipeline_frame(
        &delayed_batch[0],
        10U
    );
    assert(delayed_info.total_frames == delayed_total);
}


static void test_task_policy_keeps_services_independent(void)
{
    assert(APP_TASK_CORE_SERVICES != APP_TASK_CORE_REALTIME);
    assert(APP_TASK_PRIORITY_AD7606 > APP_TASK_PRIORITY_SEISMIC_RECORDER);
    assert(APP_TASK_PRIORITY_POWER_BUTTON > APP_TASK_PRIORITY_SEISMIC_RECORDER);
    assert(APP_TASK_PRIORITY_KEYPAD > APP_TASK_PRIORITY_SEISMIC_RECORDER);
    assert(APP_TASK_PRIORITY_LVGL > APP_TASK_PRIORITY_SEISMIC_RECORDER);
    assert(APP_TASK_CORE_SERVICES == 0);
    assert(APP_TASK_CORE_REALTIME == 1);
}

static void test_recorder_survives_expected_sd_stalls(void)
{
    const unsigned stall_ms[] = { 500U, 1000U, 2000U };
    float channels[SEISMIC_CHANNEL_COUNT];
    assert(seismic_buffer_init() == ESP_OK);
    for (size_t stall = 0U; stall < sizeof(stall_ms) / sizeof(stall_ms[0]); ++stall) {
        assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, PIPELINE_SAMPLE_RATE_HZ) == ESP_OK);
        const uint64_t duration_frames = 60U * PIPELINE_SAMPLE_RATE_HZ;
        const uint64_t stall_frames = (uint64_t)stall_ms[stall] * PIPELINE_SAMPLE_RATE_HZ / 1000U;
        const uint64_t stall_start = duration_frames - 2U * stall_frames - 200U;
        const uint64_t stop_target = stall_start + stall_frames / 2U;
        uint64_t cursor = 0U;
        uint64_t consumed = 0U;

        for (uint64_t frame = 1U; frame <= duration_frames; ++frame) {
            pipeline_channels(frame, channels);
            assert(seismic_buffer_push((int64_t)frame * 2000LL, channels) == ESP_OK);

            /* The producer continues at 500 Hz while an SD write is stalled. */
            const bool writer_stalled =
                frame >= stall_start && frame < stall_start + stall_frames;
            if (writer_stalled || consumed >= stop_target) continue;

            seismic_frame_t batch[64];
            size_t count = 0U;
            uint64_t dropped = 0U;
            seismic_buffer_info_t info = { 0 };
            const size_t permitted = (size_t)(stop_target - consumed < 64U
                ? stop_target - consumed
                : 64U);
            assert(seismic_buffer_copy_since(&cursor, batch, permitted, &count, &dropped, &info) == ESP_OK);
            assert(dropped == 0U);
            for (size_t i = 0U; i < count; ++i) {
                assert(consumed + (uint64_t)i + 1U <= stop_target);
                assert_pipeline_frame(&batch[i], consumed + (uint64_t)i + 1U);
            }
            consumed += (uint64_t)count;
        }

        /* STOP drains the precise target, despite a preceding live writer stall;
         * samples published after STOP are deliberately not part of this session. */
        while (consumed < stop_target) {
            seismic_frame_t batch[64];
            size_t count = 0U;
            uint64_t dropped = 0U;
            seismic_buffer_info_t info = { 0 };
            assert(seismic_buffer_copy_since(&cursor, batch, 64U, &count, &dropped, &info) == ESP_OK);
            assert(dropped == 0U);
            assert(count > 0U);
            for (size_t i = 0U; i < count; ++i) {
                assert(consumed + (uint64_t)i + 1U <= stop_target);
                assert_pipeline_frame(&batch[i], consumed + (uint64_t)i + 1U);
            }
            consumed += count;
        }
        assert(consumed == stop_target);
        assert(cursor == stop_target);
    }
}


int main(void)
{
    test_task_policy_keeps_services_independent();
    test_recorder_survives_expected_sd_stalls();
    test_pipeline_fan_out_preserves_signal();
    puts("seismic pipeline scheduling contract tests passed");
    return 0;
}
