#include <assert.h>
#include <stdio.h>

#include "esp_err.h"
#include "seismic_buffer.h"

static void frame_values(float channels[SEISMIC_CHANNEL_COUNT], float base)
{
    for (size_t i = 0; i < SEISMIC_CHANNEL_COUNT; ++i) {
        channels[i] = base + (float)i * 0.25f;
    }
}

static void assert_frame(const seismic_frame_t *frame, uint64_t number)
{
    float expected[SEISMIC_CHANNEL_COUNT];
    frame_values(expected, (float)number);
    assert(frame->timestamp_us == (int64_t)(number * 10U));
    for (size_t i = 0; i < SEISMIC_CHANNEL_COUNT; ++i) {
        assert(frame->channel_uv[i] == expected[i]);
    }
}

static void test_latest_and_cursor_batches(void)
{
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500) == ESP_OK);

    float channels[SEISMIC_CHANNEL_COUNT];
    for (uint64_t number = 1; number <= 3; ++number) {
        frame_values(channels, (float)number);
        assert(seismic_buffer_push((int64_t)(number * 10U), channels) == ESP_OK);
    }

    seismic_frame_t frames[4];
    size_t count = 0;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_latest(frames, 4, &count, &info) == ESP_OK);
    assert(count == 3);
    assert(info.source == SEISMIC_SOURCE_TEST);
    assert(info.sample_rate_hz == 500);
    assert(info.buffered_frames == 3);
    assert(info.total_frames == 3);
    for (size_t i = 0; i < count; ++i) {
        assert_frame(&frames[i], (uint64_t)i + 1U);
    }

    uint64_t cursor = 0;
    uint64_t dropped = 99;
    assert(seismic_buffer_copy_since(
        &cursor, frames, 2, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 2);
    assert(dropped == 0);
    assert(cursor == 2);
    assert_frame(&frames[0], 1);
    assert_frame(&frames[1], 2);

    assert(seismic_buffer_copy_since(
        &cursor, frames, 2, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 1);
    assert(cursor == 3);
    assert_frame(&frames[0], 3);

    assert(seismic_buffer_copy_since(
        &cursor, frames, 2, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 0);
    assert(dropped == 0);
}

static void test_wrap_drop_and_session_boundary(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500) == ESP_OK);

    float channels[SEISMIC_CHANNEL_COUNT];
    const uint64_t total = (uint64_t)SEISMIC_BUFFER_CAPACITY + 5U;
    for (uint64_t number = 1; number <= total; ++number) {
        frame_values(channels, (float)number);
        assert(seismic_buffer_push((int64_t)(number * 10U), channels) == ESP_OK);
    }

    seismic_frame_t frames[10];
    size_t count = 0;
    uint64_t cursor = 0;
    uint64_t dropped = 0;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_since(
        &cursor, frames, 10, &count, &dropped, &info
    ) == ESP_OK);
    assert(dropped == 5);
    assert(count == 10);
    assert(cursor == 15);
    for (size_t i = 0; i < count; ++i) {
        assert_frame(&frames[i], (uint64_t)i + 6U);
    }

    cursor = total + 1U;
    assert(seismic_buffer_copy_since(
        &cursor, frames, 10, &count, &dropped, &info
    ) == ESP_ERR_INVALID_STATE);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 250) == ESP_OK);
    assert(seismic_buffer_copy_since(
        &cursor, frames, 10, &count, &dropped, &info
    ) == ESP_ERR_INVALID_STATE);
}

static void test_argument_and_lifecycle_contracts(void)
{
    float channels[SEISMIC_CHANNEL_COUNT] = { 0 };
    seismic_frame_t frames[1];
    size_t count = 0;
    uint64_t cursor = 0;
    uint64_t dropped = 0;
    seismic_buffer_info_t info;

    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 0) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_push(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_latest(NULL, 1, &count, &info) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_latest(frames, 1, NULL, &info) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_since(
        &cursor, frames, 1, &count, &dropped, NULL
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_push(0, channels) == ESP_ERR_INVALID_STATE);
}


static void test_configured_rate_matches_published_timestamp_cadence(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);

    float channels[SEISMIC_CHANNEL_COUNT] = { 0 };
    for (size_t index = 0; index < 5U; ++index) {
        channels[0] = (float)index;
        assert(seismic_buffer_push(
            (int64_t)index * 2000,
            channels
        ) == ESP_OK);
    }

    seismic_frame_t frames[5];
    size_t count = 0;
    uint64_t cursor = 0;
    uint64_t dropped = 0;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_since(
        &cursor,
        frames,
        5,
        &count,
        &dropped,
        &info
    ) == ESP_OK);
    assert(count == 5U);
    assert(dropped == 0U);
    assert(info.source == SEISMIC_SOURCE_AD7606);
    assert(info.sample_rate_hz == 500U);

    for (size_t index = 1; index < count; ++index) {
        assert(
            frames[index].timestamp_us -
            frames[index - 1U].timestamp_us ==
            2000
        );
    }
}

int main(void)
{
    test_argument_and_lifecycle_contracts();
    test_latest_and_cursor_batches();
    test_wrap_drop_and_session_boundary();
    test_configured_rate_matches_published_timestamp_cadence();
    puts("seismic buffer tests passed");
    return 0;
}
