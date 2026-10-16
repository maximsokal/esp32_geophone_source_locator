#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "seismic_buffer.h"

#define TEST_PSRAM_BUDGET_BYTES 2097152U
#define TEST_SECONDS            2U
#define TEST_SLOT_BYTES         \
    (sizeof(seismic_frame_t) + 2U * sizeof(uint64_t) + sizeof(uint32_t))

#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
static bool s_fail_next_allocation;
static size_t s_last_allocation_size;
static size_t s_allocation_count;
static size_t s_free_count;

typedef struct
{
    size_t allocations;
    size_t frees;
} allocation_counts_t;

static allocation_counts_t allocation_counts(void)
{
    return (allocation_counts_t) {
        .allocations = s_allocation_count,
        .frees = s_free_count,
    };
}

static void assert_no_allocation_or_free_since(allocation_counts_t before)
{
    assert(s_allocation_count == before.allocations);
    assert(s_free_count == before.frees);
}
#endif
#if defined(SEISMIC_BUFFER_TEST_HOOKS)
static bool s_reconfigure_on_enter;
static bool s_reconfigure_attempted;
static esp_err_t s_reconfigure_result;
static uint32_t s_reconfigure_rate;
static bool s_inject_torn_read;
static bool s_probe_after_odd_sequence;
static bool s_probe_after_slot_publish;
static esp_err_t s_probe_read_result;
static size_t s_probe_frame_count;
static seismic_buffer_info_t s_probe_info;
static seismic_frame_t s_probe_frame;
#if defined(SEISMIC_BUFFER_TEST_BROKEN_PUBLISH)
static bool s_broken_publish_probe_armed;
#endif
#endif

static void push_number(uint64_t number);
static void assert_frame(const seismic_frame_t *frame, uint64_t number);


#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    assert((caps & MALLOC_CAP_SPIRAM) != 0U);
    assert((caps & MALLOC_CAP_8BIT) != 0U);
    s_allocation_count++;
    s_last_allocation_size = size;
    if (s_fail_next_allocation) {
        s_fail_next_allocation = false;
        return NULL;
    }

    void *memory = malloc(size);
    return memory;
}


void heap_caps_free(void *memory)
{
    if (memory != NULL) {
        ++s_free_count;
        free(memory);
    }
}
#endif


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
void seismic_buffer_test_enter_critical(portMUX_TYPE *lock)
{
    (void)lock;
    if (s_reconfigure_on_enter) {
        s_reconfigure_on_enter = false;
        s_reconfigure_attempted = true;
        s_reconfigure_result = seismic_buffer_configure(
            SEISMIC_SOURCE_TEST,
            s_reconfigure_rate
        );
    }
}


void seismic_buffer_test_after_frame_tag(size_t slot_index)
{
    (void)slot_index;
    if (!s_inject_torn_read) {
        return;
    }

    s_inject_torn_read = false;
    for (uint64_t number = 2U; number <= 2049U; ++number) {
        push_number(number);
    }
}

static void seismic_buffer_test_probe_reader(void)
{
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    s_probe_frame_count = 0U;
    s_probe_info = (seismic_buffer_info_t) { 0 };
    s_probe_read_result = seismic_buffer_copy_since(
        &cursor,
        &s_probe_frame,
        1U,
        &s_probe_frame_count,
        &dropped,
        &s_probe_info
    );
}

void seismic_buffer_test_after_odd_sequence(size_t slot_index)
{
    (void)slot_index;
    if (s_probe_after_odd_sequence) {
        s_probe_after_odd_sequence = false;
        seismic_buffer_test_probe_reader();
    }
}

void seismic_buffer_test_after_slot_publish(size_t slot_index)
{
    (void)slot_index;
    if (s_probe_after_slot_publish) {
        s_probe_after_slot_publish = false;
        seismic_buffer_test_probe_reader();
    }
}

#if defined(SEISMIC_BUFFER_TEST_BROKEN_PUBLISH)
void seismic_buffer_test_broken_publish_probe(size_t slot_index)
{
    (void)slot_index;
    if (!s_broken_publish_probe_armed) {
        return;
    }
    s_broken_publish_probe_armed = false;
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    seismic_frame_t frame;
    assert(seismic_buffer_copy_since(
        &cursor, &frame, 1U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 1U && info.total_frames == 2048U);
    assert_frame(&frame, 1U); /* This must fail when publication is broken. */
}
#endif
#endif


static void frame_values(float channels[SEISMIC_CHANNEL_COUNT], float base)
{
    for (size_t i = 0; i < SEISMIC_CHANNEL_COUNT; ++i) {
        channels[i] = base + (float)i * 0.25f;
    }
}


static void push_number(uint64_t number)
{
    float channels[SEISMIC_CHANNEL_COUNT];
    frame_values(channels, (float)number);
    assert(seismic_buffer_push((int64_t)(number * 10U), channels) == ESP_OK);
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


#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
static void test_allocation_failures(void)
{
    s_fail_next_allocation = true;
    assert(seismic_buffer_init() == ESP_ERR_NO_MEM);
    assert(seismic_buffer_capacity() == 0U);

    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_capacity() == 2048U);
    assert(seismic_buffer_init() == ESP_OK);
    assert(s_last_allocation_size == 2048U * TEST_SLOT_BYTES);
    assert(s_last_allocation_size <= TEST_PSRAM_BUDGET_BYTES);

    float channels[SEISMIC_CHANNEL_COUNT] = { 0 };
    seismic_frame_t frames[1];
    size_t count = 0U;
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 0U) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_push(0, channels) == ESP_ERR_INVALID_STATE);
    assert(seismic_buffer_push(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_latest(NULL, 1U, &count, &info) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_latest(frames, 1U, NULL, &info) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_latest(frames, 0U, &count, &info) == ESP_ERR_INVALID_ARG);
    assert(seismic_buffer_copy_since(
        &cursor, frames, 1U, &count, &dropped, NULL
    ) == ESP_ERR_INVALID_ARG);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    const size_t allocations_before_same_capacity_rate_change = s_allocation_count;
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 250U) == ESP_OK);
    assert(s_allocation_count == allocations_before_same_capacity_rate_change + 1U);
    assert(seismic_buffer_capacity() == 2048U);
    s_fail_next_allocation = true;
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 5000U) == ESP_ERR_NO_MEM);
    assert(seismic_buffer_capacity() == 0U);

    assert(seismic_buffer_push(10, channels) == ESP_ERR_INVALID_STATE);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 5000U) == ESP_OK);
    assert(seismic_buffer_capacity() == 10000U);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
    const size_t frees_before_stop = s_free_count;
    assert(seismic_buffer_configure(SEISMIC_SOURCE_NONE, 0U) == ESP_OK);
    assert(seismic_buffer_capacity() == 0U);
    assert(s_free_count == frees_before_stop + 1U);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
}
#endif


static void test_capacity_by_rate_and_budget(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    assert(seismic_buffer_capacity() == 2048U);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 5000U) == ESP_OK);
    assert(seismic_buffer_capacity() == 10000U);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
    assert(seismic_buffer_capacity() == 40000U);

    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 40000U) == ESP_OK);
    const size_t budget_cap = TEST_PSRAM_BUDGET_BYTES / TEST_SLOT_BYTES;
    assert(seismic_buffer_capacity() == budget_cap);
    assert(budget_cap == 47662U);
    assert(seismic_buffer_capacity() < 40000U * TEST_SECONDS);
    assert(budget_cap * TEST_SLOT_BYTES <= TEST_PSRAM_BUDGET_BYTES);
}


static void test_latest_and_cursor_batches(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    push_number(1U);
    push_number(2U);
    push_number(3U);

    seismic_frame_t frames[4];
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_latest(frames, 4U, &count, &info) == ESP_OK);
    assert(count == 3U);
    assert(info.source == SEISMIC_SOURCE_TEST);
    assert(info.sample_rate_hz == 500U);
    assert(info.buffered_frames == 3U);
    assert(info.total_frames == 3U);
    for (size_t i = 0; i < count; ++i) {
        assert_frame(&frames[i], (uint64_t)i + 1U);
    }

    uint64_t cursor = 0U;
    uint64_t dropped = 99U;
    assert(seismic_buffer_copy_since(
        &cursor, frames, 2U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 2U);
    assert(dropped == 0U);
    assert(cursor == 2U);
    assert_frame(&frames[0], 1U);
    assert_frame(&frames[1], 2U);

    assert(seismic_buffer_copy_since(
        &cursor, frames, 2U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 1U);
    assert(cursor == 3U);
    assert_frame(&frames[0], 3U);

    assert(seismic_buffer_copy_since(
        &cursor, frames, 2U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 0U);
    assert(dropped == 0U);
}


static void test_runtime_wrap_and_skipped_frames(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 5000U) == ESP_OK);
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    const allocation_counts_t before = allocation_counts();
#endif
    const uint64_t capacity = seismic_buffer_capacity();
    const uint64_t total = capacity + 5U;
    for (uint64_t number = 1U; number <= total; ++number) {
        push_number(number);
    }

    seismic_frame_t frames[10];
    uint64_t source_sample_indices[10];
    size_t count = 0U;
    uint64_t cursor = capacity - 2U;
    uint64_t dropped = 99U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, source_sample_indices, 10U, &count, &dropped, &info
    ) == ESP_OK);
    assert(dropped == 0U);
    assert(count == 7U);
    assert(cursor == total);
    assert_frame(&frames[0], capacity - 1U);
    assert_frame(&frames[1], capacity);
    assert_frame(&frames[2], capacity + 1U);
    assert_frame(&frames[6], total);
    assert(source_sample_indices[0] == capacity - 2U);

    cursor = 0U;
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, source_sample_indices, 10U, &count, &dropped, &info
    ) == ESP_OK);
    assert(dropped == 5U);
    assert(count == 10U);
    assert(cursor == 15U);
    assert_frame(&frames[0], 6U);
    assert_frame(&frames[9], 15U);
    assert(source_sample_indices[0] == 5U);
    assert(source_sample_indices[9] == 14U);

    cursor = total + 1U;
    assert(seismic_buffer_copy_since(
        &cursor, frames, 10U, &count, &dropped, &info
    ) == ESP_ERR_INVALID_STATE);

#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    assert_no_allocation_or_free_since(before);
#endif
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 250U) == ESP_OK);
    assert(seismic_buffer_copy_since(
        &cursor, frames, 10U, &count, &dropped, &info
    ) == ESP_ERR_INVALID_STATE);
}


static void test_wrap_boundary_counts(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    const uint64_t capacity = seismic_buffer_capacity();
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    const allocation_counts_t before = allocation_counts();
#endif

    for (uint64_t number = 1U; number < capacity; ++number) {
        push_number(number);
    }

    seismic_frame_t frame;
    size_t count = 0U;
    seismic_buffer_info_t info = {0};
    assert(seismic_buffer_copy_latest(&frame, 1U, &count, &info) == ESP_OK);
    assert(count == 1U && info.total_frames == capacity - 1U);
    assert_frame(&frame, capacity - 1U);

    push_number(capacity);
    uint64_t cursor = capacity - 2U;
    uint64_t dropped = 0U;
    seismic_frame_t pair[2];
    assert(seismic_buffer_copy_since(
        &cursor, pair, 2U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 2U && dropped == 0U && info.total_frames == capacity);
    assert_frame(&pair[0], capacity - 1U);
    assert_frame(&pair[1], capacity);

    push_number(capacity + 1U);
    cursor = capacity - 1U;
    assert(seismic_buffer_copy_since(
        &cursor, pair, 2U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 2U && dropped == 0U && info.total_frames == capacity + 1U);
    assert_frame(&pair[0], capacity);
    assert_frame(&pair[1], capacity + 1U);

#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    assert_no_allocation_or_free_since(before);
#endif
}


static void test_queue_stats_track_n_boundaries_and_reader_drops(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    const uint64_t capacity = seismic_buffer_capacity();
    seismic_buffer_stats_t stats = {0};
    assert(seismic_buffer_get_stats(&stats) == ESP_OK);
    assert(stats.peak_occupancy == 0U && stats.dropped_frames_total == 0U);

    for (uint64_t number = 1U; number < capacity; ++number) {
        push_number(number);
    }
    assert(seismic_buffer_get_stats(&stats) == ESP_OK);
    assert(stats.peak_occupancy == capacity - 1U);

    push_number(capacity);
    assert(seismic_buffer_get_stats(&stats) == ESP_OK);
    assert(stats.peak_occupancy == capacity);
    push_number(capacity + 1U);

    seismic_frame_t frame;
    size_t count = 0U;
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    seismic_buffer_info_t info = {0};
    assert(seismic_buffer_copy_since(
        &cursor, &frame, 1U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 1U && dropped == 1U);
    assert(seismic_buffer_get_stats(&stats) == ESP_OK);
    assert(stats.peak_occupancy == capacity);
    assert(stats.dropped_frames_total == 1U);
}


static void test_indexed_source_gaps_are_preserved(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    const allocation_counts_t before = allocation_counts();
#endif
    const uint64_t indexes[] = {0U, 1U, 5U, 6U};
    float channels[SEISMIC_CHANNEL_COUNT] = {0};
    for (size_t index = 0U; index < sizeof(indexes) / sizeof(indexes[0]); ++index) {
        assert(seismic_buffer_push_indexed(
            (int64_t)indexes[index] * 2000LL,
            indexes[index],
            channels
        ) == ESP_OK);
    }

    seismic_frame_t frames[4];
    uint64_t source_sample_indices[4] = {0};
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    size_t count = 0U;
    seismic_buffer_info_t info = {0};
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, source_sample_indices, 4U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 4U && dropped == 0U);
    for (size_t index = 0U; index < count; ++index) {
        assert(source_sample_indices[index] == indexes[index]);
        assert(frames[index].timestamp_us == (int64_t)indexes[index] * 2000LL);
    }
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    assert_no_allocation_or_free_since(before);
#endif
}


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
static void test_sync_operation_baseline(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    seismic_buffer_test_counters_reset();
    push_number(1U);

    seismic_buffer_test_counters_t counters;
    seismic_buffer_test_counters_get(&counters);
    assert(counters.operation_enter_attempts == 1U);
    assert(counters.operation_cas_retries == 0U);
    assert(counters.operation_exit_decrements == 1U);
    assert(counters.publish_sequence_loads == 1U);
    assert(counters.publish_sequence_stores == 2U);
    assert(counters.metadata_critical_entries == 2U);
    printf(
        "MAX-372 seismic baseline per push: enter=%llu cas_retry=%llu "
        "exit=%llu seq_load=%llu seq_stores=%llu metadata_critical=%llu "
        "capacity_loads=%llu\n",
        (unsigned long long)counters.operation_enter_attempts,
        (unsigned long long)counters.operation_cas_retries,
        (unsigned long long)counters.operation_exit_decrements,
        (unsigned long long)counters.publish_sequence_loads,
        (unsigned long long)counters.publish_sequence_stores,
        (unsigned long long)counters.metadata_critical_entries,
        (unsigned long long)counters.capacity_loads
    );
}


static void test_metadata_snapshot_at_publish_boundary(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    s_probe_after_slot_publish = true;
    push_number(1U);
    assert(!s_probe_after_slot_publish);
    assert(s_probe_read_result == ESP_OK);
    assert(s_probe_frame_count == 0U);
    assert(s_probe_info.total_frames == 0U);

    seismic_frame_t frame;
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_latest(&frame, 1U, &count, &info) == ESP_OK);
    assert(count == 1U && info.total_frames == 1U);
    assert_frame(&frame, 1U);
}


static void test_reader_retries_while_producer_is_odd(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    const size_t capacity = seismic_buffer_capacity();
    assert(capacity == 2048U);
    for (uint64_t number = 1U; number <= capacity; ++number) {
        push_number(number);
    }

    seismic_buffer_test_counters_reset();
    s_probe_after_odd_sequence = true;
    push_number((uint64_t)capacity + 1U);
    assert(!s_probe_after_odd_sequence);
    assert(s_probe_read_result == ESP_ERR_TIMEOUT);
    assert(s_probe_frame_count == 0U);
    assert(s_probe_info.total_frames == capacity);

    seismic_buffer_test_counters_t counters;
    seismic_buffer_test_counters_get(&counters);
    assert(counters.read_retries_odd >= 4U);
    assert(counters.producer_reader_conflicts >= 4U);
    assert(counters.snapshot_retries > 0U);

    seismic_frame_t latest;
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_latest(&latest, 1U, &count, &info) == ESP_OK);
    assert(count == 1U && info.total_frames == capacity + 1U);
    assert_frame(&latest, capacity + 1U);
}


static void test_indexed_source_gaps_through_wrap(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    const uint64_t capacity = seismic_buffer_capacity();
    float channels[SEISMIC_CHANNEL_COUNT];
    for (uint64_t number = 1U; number <= capacity + 3U; ++number) {
        frame_values(channels, (float)number);
        assert(seismic_buffer_push_indexed(
            (int64_t)(number * 10U), number * 3U, channels
        ) == ESP_OK);
    }

    uint64_t cursor = capacity - 2U;
    uint64_t indexes[5] = { 0U };
    uint64_t dropped = 0U;
    size_t count = 0U;
    seismic_frame_t frames[5];
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, indexes, 5U, &count, &dropped, &info
    ) == ESP_OK);
    assert(count == 5U && dropped == 0U && info.total_frames == capacity + 3U);
    for (size_t i = 0U; i < count; ++i) {
        const uint64_t number = capacity - 1U + (uint64_t)i;
        assert(indexes[i] == number * 3U);
        assert_frame(&frames[i], number);
    }
}


static void test_high_rate_capacities_wrap(void)
{
    const uint32_t rates[] = { 5000U, 20000U };
    const size_t capacities[] = { 10000U, 40000U };
    for (size_t rate = 0U; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
        assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, rates[rate]) == ESP_OK);
        assert(seismic_buffer_capacity() == capacities[rate]);
        float channels[SEISMIC_CHANNEL_COUNT];
        for (uint64_t number = 1U; number <= capacities[rate] + 1U; ++number) {
            frame_values(channels, (float)number);
            assert(seismic_buffer_push_indexed(
                (int64_t)(number * 10U), number * 5U, channels
            ) == ESP_OK);
        }
        seismic_frame_t frame;
        uint64_t cursor = capacities[rate];
        uint64_t index = 0U;
        uint64_t dropped = 0U;
        size_t count = 0U;
        seismic_buffer_info_t info = { 0 };
        assert(seismic_buffer_copy_since_with_source_indices(
            &cursor, &frame, &index, 1U, &count, &dropped, &info
        ) == ESP_OK);
        assert(count == 1U && dropped == 0U);
        assert(index == (capacities[rate] + 1U) * 5U);
        assert_frame(&frame, capacities[rate] + 1U);
    }
}
#endif


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
static void test_reconfigure_rejected_during_producer_and_consumer(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    const allocation_counts_t before = allocation_counts();
#endif

    s_reconfigure_rate = 5000U;
    s_reconfigure_attempted = false;
    s_reconfigure_on_enter = true;
    push_number(1U);
    assert(s_reconfigure_attempted);
    assert(s_reconfigure_result == ESP_ERR_INVALID_STATE);
    assert(seismic_buffer_capacity() == 2048U);

    seismic_frame_t frame;
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    s_reconfigure_attempted = false;
    s_reconfigure_on_enter = true;
    assert(seismic_buffer_copy_latest(&frame, 1U, &count, &info) == ESP_OK);
    assert(s_reconfigure_attempted);
    assert(s_reconfigure_result == ESP_ERR_INVALID_STATE);
    assert(seismic_buffer_capacity() == 2048U);

#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    assert_no_allocation_or_free_since(before);
#endif

    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 5000U) == ESP_OK);
    assert(seismic_buffer_capacity() == 10000U);
}
#endif


static void test_configured_rate_timestamp_cadence(void)
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
    size_t count = 0U;
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    seismic_buffer_info_t info = { 0 };
    assert(seismic_buffer_copy_since(
        &cursor,
        frames,
        5U,
        &count,
        &dropped,
        &info
    ) == ESP_OK);
    assert(count == 5U);
    assert(dropped == 0U);
    assert(info.source == SEISMIC_SOURCE_AD7606);
    assert(info.sample_rate_hz == 500U);

    for (size_t index = 1U; index < count; ++index) {
        assert(
            frames[index].timestamp_us -
            frames[index - 1U].timestamp_us ==
            2000
        );
    }
}


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
static void assert_frame_coherent(const seismic_frame_t *frame)
{
    assert(frame->timestamp_us > 0);
    assert(frame->timestamp_us % 10 == 0);
    const float base = (float)(frame->timestamp_us / 10);
    assert(frame->channel_uv[0] == base);
    assert(frame->channel_uv[1] == base + 0.25f);
    assert(frame->channel_uv[2] == base + 0.5f);
    assert(frame->channel_uv[3] == base + 0.75f);
}
#endif


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
static void test_concurrent_sequence_validation(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    const allocation_counts_t before = allocation_counts();
#endif
    push_number(1U);

    seismic_buffer_test_counters_reset();

    seismic_frame_t frames[1];
    size_t count = 0U;
    seismic_buffer_info_t info = { 0 };
    s_inject_torn_read = true;
    assert(seismic_buffer_copy_latest(frames, 1U, &count, &info) == ESP_OK);
    assert(count == 1U);
    assert(info.total_frames == 2049U);
    assert_frame(&frames[0], 2049U);
    assert_frame_coherent(&frames[0]);
    seismic_buffer_test_counters_t counters;
    seismic_buffer_test_counters_get(&counters);
    assert(counters.read_retries_changed > 0U);
    assert(counters.snapshot_retries > 0U);
    assert(counters.producer_reader_conflicts > 0U);
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    assert_no_allocation_or_free_since(before);
#endif
}
#endif


int main(void)
{
#if defined(SEISMIC_BUFFER_TEST_BROKEN_PUBLISH)
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_TEST, 500U) == ESP_OK);
    for (uint64_t number = 1U; number <= 2048U; ++number) {
        push_number(number);
    }
    s_broken_publish_probe_armed = true;
    push_number(2049U);
    assert(false && "broken publish fixture unexpectedly passed");
    return 1;
#else
#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
    test_allocation_failures();
#else
    assert(seismic_buffer_init() == ESP_OK);
#endif
    test_capacity_by_rate_and_budget();
    test_latest_and_cursor_batches();
    test_runtime_wrap_and_skipped_frames();
    test_wrap_boundary_counts();
    test_queue_stats_track_n_boundaries_and_reader_drops();
    test_indexed_source_gaps_are_preserved();
#if defined(SEISMIC_BUFFER_TEST_HOOKS)
    test_sync_operation_baseline();
    test_metadata_snapshot_at_publish_boundary();
    test_reader_retries_while_producer_is_odd();
    test_indexed_source_gaps_through_wrap();
    test_high_rate_capacities_wrap();
    test_reconfigure_rejected_during_producer_and_consumer();
#endif
    test_configured_rate_timestamp_cadence();
#if defined(SEISMIC_BUFFER_TEST_HOOKS)
    test_concurrent_sequence_validation();
#endif
    puts("seismic buffer tests passed");
    return 0;
#endif
}
