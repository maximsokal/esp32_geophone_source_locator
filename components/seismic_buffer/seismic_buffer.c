#include "seismic_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#ifndef CONFIG_SEISMIC_BUFFER_SECONDS
#define CONFIG_SEISMIC_BUFFER_SECONDS 2
#endif

#ifndef CONFIG_SEISMIC_BUFFER_PSRAM_BUDGET_BYTES
#define CONFIG_SEISMIC_BUFFER_PSRAM_BUDGET_BYTES (2U * 1024U * 1024U)
#endif


/* ESP-IDF builds use GCC's C11-compatible atomic builtins. MSVC does not
 * expose those builtins, so keep the host contract on the same sequentially
 * consistent 32-bit operations through its interlocked primitives. */
#if defined(_MSC_VER)
#include <intrin.h>

static uint32_t seismic_sequence_load(const volatile uint32_t *sequence)
{
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)sequence,
        0L,
        0L
    );
}

static void seismic_sequence_store(
    volatile uint32_t *sequence,
    uint32_t value
)
{
    (void)_InterlockedExchange(
        (volatile long *)sequence,
        (long)value
    );
}
#else

static uint32_t seismic_sequence_load(const volatile uint32_t *sequence)
{
    return __atomic_load_n(sequence, __ATOMIC_SEQ_CST);
}

static void seismic_sequence_store(
    volatile uint32_t *sequence,
    uint32_t value
)
{
    __atomic_store_n(sequence, value, __ATOMIC_SEQ_CST);
}
#endif


#if defined(_MSC_VER)
static bool seismic_state_compare_exchange(
    volatile uint32_t *state,
    uint32_t *expected,
    uint32_t desired
)
{
    const uint32_t observed = (uint32_t)_InterlockedCompareExchange(
        (volatile long *)state,
        (long)desired,
        (long)*expected
    );
    if (observed == *expected) {
        return true;
    }
    *expected = observed;
    return false;
}

static void seismic_state_store(volatile uint32_t *state, uint32_t value)
{
    (void)_InterlockedExchange((volatile long *)state, (long)value);
}

static uint32_t seismic_state_load(const volatile uint32_t *state)
{
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)state,
        0L,
        0L
    );
}

static void seismic_state_decrement(volatile uint32_t *state)
{
    (void)_InterlockedExchangeAdd((volatile long *)state, -1L);
}
#else
static bool seismic_state_compare_exchange(
    volatile uint32_t *state,
    uint32_t *expected,
    uint32_t desired
)
{
    return __atomic_compare_exchange_n(
        state,
        expected,
        desired,
        false,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST
    );
}

static void seismic_state_store(volatile uint32_t *state, uint32_t value)
{
    __atomic_store_n(state, value, __ATOMIC_SEQ_CST);
}

static uint32_t seismic_state_load(const volatile uint32_t *state)
{
    return __atomic_load_n(state, __ATOMIC_SEQ_CST);
}

static void seismic_state_decrement(volatile uint32_t *state)
{
    (void)__atomic_sub_fetch(state, 1U, __ATOMIC_SEQ_CST);
}
#endif


#define SEISMIC_SLOT_READ_RETRIES      4
#define SEISMIC_SNAPSHOT_RETRIES       4
#define SEISMIC_MINIMUM_CAPACITY       2048U
#define SEISMIC_SLOT_BYTES             \
    (sizeof(seismic_frame_t) + (2U * sizeof(uint64_t)) + sizeof(uint32_t))
#define SEISMIC_CONFIGURING            (1U << 31)
#define SEISMIC_ACTIVE_MASK            (SEISMIC_CONFIGURING - 1U)

#if defined(SEISMIC_BUFFER_TEST_HOOKS)
void seismic_buffer_test_after_frame_tag(size_t slot_index);
void seismic_buffer_test_after_odd_sequence(size_t slot_index);
void seismic_buffer_test_after_slot_publish(size_t slot_index);
#if defined(SEISMIC_BUFFER_TEST_BROKEN_PUBLISH)
void seismic_buffer_test_broken_publish_probe(size_t slot_index);
#endif
static seismic_buffer_test_counters_t s_test_counters;
#define SEISMIC_TEST_COUNT(field) (++s_test_counters.field)

void seismic_buffer_test_counters_reset(void)
{
    memset(&s_test_counters, 0, sizeof(s_test_counters));
}

void seismic_buffer_test_counters_get(seismic_buffer_test_counters_t *out)
{
    if (out != NULL) {
        *out = s_test_counters;
    }
}
#else
#define SEISMIC_TEST_COUNT(field) ((void)0)
#endif


typedef struct
{
    size_t frame_count;
    uint64_t total_frames;
    uint32_t sample_rate_hz;
    seismic_source_t source;

} seismic_metadata_t;


static portMUX_TYPE s_metadata_lock =
    portMUX_INITIALIZER_UNLOCKED;

/* One PSRAM allocation: frames, ring tags, source indexes, then sequences. */
static void *s_storage = NULL;
static seismic_frame_t *s_frames = NULL;
static uint64_t *s_frame_numbers = NULL;
static uint64_t *s_source_sample_indices = NULL;
static volatile uint32_t *s_sequences = NULL;
static volatile uint32_t s_capacity = 0;
static volatile uint32_t s_operation_state = 0;

static size_t s_write_index = 0;
static size_t s_frame_count = 0;
static size_t s_peak_frame_count = 0;
static uint64_t s_total_frames = 0;
static uint64_t s_dropped_frames_total = 0;
static uint32_t s_sample_rate_hz = 0;

static seismic_source_t s_source =
    SEISMIC_SOURCE_NONE;

static bool s_initialized = false;


static uint64_t seismic_buffer_saturating_add_u64(uint64_t value, uint64_t amount)
{
    return amount > UINT64_MAX - value ? UINT64_MAX : value + amount;
}


static bool seismic_buffer_enter_operation(void)
{
    SEISMIC_TEST_COUNT(operation_enter_attempts);
    uint32_t state = seismic_state_load(&s_operation_state);
    for (;;) {
        if ((state & SEISMIC_CONFIGURING) != 0U ||
            (state & SEISMIC_ACTIVE_MASK) == SEISMIC_ACTIVE_MASK) {
            return false;
        }

        uint32_t expected = state;
        if (seismic_state_compare_exchange(
                &s_operation_state,
                &expected,
                state + 1U
            )) {
            return true;
        }
        SEISMIC_TEST_COUNT(operation_cas_retries);
        state = expected;
    }
}


static bool seismic_buffer_begin_configuration(void)
{
    uint32_t expected = 0U;
    return seismic_state_compare_exchange(
        &s_operation_state,
        &expected,
        SEISMIC_CONFIGURING
    );
}


static void seismic_buffer_leave_operation(void)
{
    SEISMIC_TEST_COUNT(operation_exit_decrements);
    seismic_state_decrement(&s_operation_state);
}


static void seismic_buffer_end_configuration(void)
{
    seismic_state_store(&s_operation_state, 0U);
}


static esp_err_t seismic_buffer_capacity_for_rate(
    uint32_t sample_rate_hz,
    size_t *capacity
)
{
    if (sample_rate_hz == 0U || capacity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t budget_frames =
        (size_t)CONFIG_SEISMIC_BUFFER_PSRAM_BUDGET_BYTES /
        SEISMIC_SLOT_BYTES;
    if (budget_frames < SEISMIC_MINIMUM_CAPACITY) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t requested =
        (uint64_t)sample_rate_hz *
        (uint64_t)CONFIG_SEISMIC_BUFFER_SECONDS;
    if (requested < SEISMIC_MINIMUM_CAPACITY) {
        requested = SEISMIC_MINIMUM_CAPACITY;
    }
    if (requested > (uint64_t)budget_frames) {
        requested = (uint64_t)budget_frames;
    }

    *capacity = (size_t)requested;
    return ESP_OK;
}


static esp_err_t seismic_buffer_storage_size(
    size_t capacity,
    size_t *bytes
)
{
    if (capacity == 0U || bytes == NULL ||
        capacity > SIZE_MAX / SEISMIC_SLOT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    *bytes = capacity * SEISMIC_SLOT_BYTES;
    return ESP_OK;
}


static void seismic_buffer_set_storage(void *storage, size_t capacity)
{
    uint8_t *base = (uint8_t *)storage;
    const size_t frames_bytes = capacity * sizeof(seismic_frame_t);
    const size_t numbers_bytes = capacity * sizeof(uint64_t);

    s_storage = storage;
    s_frames = (seismic_frame_t *)base;
    s_frame_numbers = (uint64_t *)(base + frames_bytes);
    s_source_sample_indices = (uint64_t *)(base + frames_bytes + numbers_bytes);
    s_sequences = (volatile uint32_t *)(base + frames_bytes + 2U * numbers_bytes);
    seismic_state_store(&s_capacity, (uint32_t)capacity);
}


static void seismic_buffer_clear_storage(void)
{
    if (s_storage != NULL) {
        heap_caps_free(s_storage);
    }
    s_storage = NULL;
    s_frames = NULL;
    s_frame_numbers = NULL;
    s_source_sample_indices = NULL;
    s_sequences = NULL;
    seismic_state_store(&s_capacity, 0U);
}


static void seismic_buffer_reset_metadata(
    seismic_source_t source,
    uint32_t sample_rate_hz
)
{
    SEISMIC_TEST_COUNT(metadata_critical_entries);
    portENTER_CRITICAL(&s_metadata_lock);
    s_source = source;
    s_sample_rate_hz = sample_rate_hz;
    s_write_index = 0U;
    s_frame_count = 0U;
    s_peak_frame_count = 0U;
    s_total_frames = 0U;
    s_dropped_frames_total = 0U;
    portEXIT_CRITICAL(&s_metadata_lock);
}


static void seismic_buffer_snapshot_metadata(
    seismic_metadata_t *metadata
)
{
    SEISMIC_TEST_COUNT(metadata_critical_entries);
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    metadata->frame_count =
        s_frame_count;

    metadata->total_frames =
        s_total_frames;

    metadata->sample_rate_hz =
        s_sample_rate_hz;

    metadata->source =
        s_source;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );
}


static void seismic_buffer_publish_info(
    const seismic_metadata_t *metadata,
    seismic_buffer_info_t *info
)
{
    info->source =
        metadata->source;

    info->sample_rate_hz =
        metadata->sample_rate_hz;

    info->buffered_frames =
        metadata->frame_count;

    info->total_frames =
        metadata->total_frames;
}


static void seismic_buffer_write_slot(
    size_t slot_index,
    uint64_t frame_number,
    uint64_t source_sample_index,
    int64_t timestamp_us,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    SEISMIC_TEST_COUNT(publish_sequence_loads);
    uint32_t sequence =
        seismic_sequence_load(
            &s_sequences[slot_index]
        );

    if ((sequence & 1U) != 0U) {
        sequence++;
    }

    /*
     * Odd sequence means the producer is mutating this slot.
     * Readers never wait for the producer; they simply retry.
     */
    SEISMIC_TEST_COUNT(publish_sequence_stores);
    seismic_sequence_store(
        &s_sequences[slot_index],
        sequence + 1U
    );

#if defined(SEISMIC_BUFFER_TEST_HOOKS)
    seismic_buffer_test_after_odd_sequence(slot_index);
#endif

#if defined(SEISMIC_BUFFER_TEST_BROKEN_PUBLISH)
    /* Test fixture only: publish an even sequence before slot contents. */
    SEISMIC_TEST_COUNT(publish_sequence_stores);
    seismic_sequence_store(&s_sequences[slot_index], sequence + 2U);
    s_frames[slot_index].timestamp_us = timestamp_us;
    s_frames[slot_index].channel_uv[0] = channel_uv[0];
    seismic_buffer_test_broken_publish_probe(slot_index);
    s_frame_numbers[slot_index] = frame_number;
    s_source_sample_indices[slot_index] = source_sample_index;
    for (size_t channel = 1U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        s_frames[slot_index].channel_uv[channel] = channel_uv[channel];
    }
    return;
#else

    s_frame_numbers[slot_index] =
        frame_number;
    s_source_sample_indices[slot_index] = source_sample_index;

    s_frames[slot_index].timestamp_us =
        timestamp_us;

    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        s_frames[slot_index].channel_uv[channel] =
            channel_uv[channel];
    }

    /*
     * Publishing an even sequence releases the completed frame.
     */
    SEISMIC_TEST_COUNT(publish_sequence_stores);
    seismic_sequence_store(
        &s_sequences[slot_index],
        sequence + 2U
    );
#if defined(SEISMIC_BUFFER_TEST_HOOKS)
    seismic_buffer_test_after_slot_publish(slot_index);
#endif
#endif
}


static bool seismic_buffer_read_slot(
    size_t slot_index,
    uint64_t expected_frame_number,
    seismic_frame_t *frame,
    uint64_t *source_sample_index
)
{
    for (
        unsigned attempt = 0;
        attempt < SEISMIC_SLOT_READ_RETRIES;
        ++attempt
    ) {
        SEISMIC_TEST_COUNT(read_sequence_loads);
        const uint32_t before =
            seismic_sequence_load(
                &s_sequences[slot_index]
            );

        if ((before & 1U) != 0U) {
            SEISMIC_TEST_COUNT(read_retries_odd);
            SEISMIC_TEST_COUNT(producer_reader_conflicts);
            continue;
        }

        const uint64_t frame_number =
            s_frame_numbers[slot_index];
        const uint64_t candidate_source_sample_index =
            s_source_sample_indices[slot_index];

#if defined(SEISMIC_BUFFER_TEST_HOOKS)
        seismic_buffer_test_after_frame_tag(slot_index);
#endif

        const seismic_frame_t candidate =
            s_frames[slot_index];

        SEISMIC_TEST_COUNT(read_sequence_loads);
        const uint32_t after =
            seismic_sequence_load(
                &s_sequences[slot_index]
            );

        if (
            before == after &&
            (after & 1U) == 0U &&
            frame_number == expected_frame_number
        ) {
            *frame =
                candidate;
            if (source_sample_index != NULL) {
                *source_sample_index = candidate_source_sample_index;
            }

            return true;
        }

        if (before != after || (after & 1U) != 0U) {
            SEISMIC_TEST_COUNT(read_retries_changed);
            SEISMIC_TEST_COUNT(producer_reader_conflicts);
        }
        else if (frame_number != expected_frame_number) {
            SEISMIC_TEST_COUNT(read_tag_conflicts);
            SEISMIC_TEST_COUNT(producer_reader_conflicts);
        }
    }

    return false;
}


esp_err_t seismic_buffer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    size_t bytes = 0U;
    esp_err_t err = seismic_buffer_storage_size(
        SEISMIC_MINIMUM_CAPACITY,
        &bytes
    );
    if (err != ESP_OK ||
        (size_t)CONFIG_SEISMIC_BUFFER_PSRAM_BUDGET_BYTES < bytes) {
        return ESP_ERR_NO_MEM;
    }

    void *storage = heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(storage, 0, bytes);
    seismic_buffer_set_storage(storage, SEISMIC_MINIMUM_CAPACITY);

    s_write_index =
        0;

    s_frame_count =
        0;

    s_total_frames =
        0;

    s_sample_rate_hz =
        0;

    s_source =
        SEISMIC_SOURCE_NONE;

    s_initialized =
        true;

    return ESP_OK;
}


size_t seismic_buffer_capacity(void)
{
    SEISMIC_TEST_COUNT(capacity_loads);
    return (size_t)seismic_state_load(&s_capacity);
}


esp_err_t seismic_buffer_get_stats(seismic_buffer_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_metadata_lock);
    stats->peak_occupancy = s_peak_frame_count;
    stats->dropped_frames_total = s_dropped_frames_total;
    portEXIT_CRITICAL(&s_metadata_lock);
    return ESP_OK;
}


esp_err_t seismic_buffer_configure(
    seismic_source_t source,
    uint32_t sample_rate_hz
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        source != SEISMIC_SOURCE_NONE &&
        sample_rate_hz == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_buffer_begin_configuration()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    size_t target_capacity = seismic_buffer_capacity();
    if (sample_rate_hz != 0U) {
        err = seismic_buffer_capacity_for_rate(
            sample_rate_hz,
            &target_capacity
        );
        if (err != ESP_OK) {
            goto done;
        }
    }

    if (sample_rate_hz == 0U) {
        /* Configuration owns the operation gate, so producer and readers
         * are quiescent before the acquisition ring is released. */
        seismic_buffer_clear_storage();
        seismic_buffer_reset_metadata(source, 0U);
        goto done;
    }

    if (sample_rate_hz != 0U &&
        (sample_rate_hz != s_sample_rate_hz ||
         target_capacity != seismic_buffer_capacity() || s_storage == NULL)) {
        size_t bytes = 0U;
        err = seismic_buffer_storage_size(target_capacity, &bytes);
        if (err != ESP_OK) {
            goto done;
        }

        /* Reconfiguration only runs with no producer or reader using slots. */
        seismic_buffer_clear_storage();
        void *storage = heap_caps_malloc(
            bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (storage == NULL) {
            seismic_buffer_reset_metadata(SEISMIC_SOURCE_NONE, 0U);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        memset(storage, 0, bytes);
        seismic_buffer_set_storage(storage, target_capacity);
    }

    seismic_buffer_reset_metadata(source, sample_rate_hz);

done:
    seismic_buffer_end_configuration();
    return err;
}


static esp_err_t seismic_buffer_push_impl(
    int64_t timestamp_us,
    uint64_t source_sample_index,
    bool has_source_sample_index,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel_uv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_buffer_enter_operation()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_storage == NULL || seismic_buffer_capacity() == 0U) {
        seismic_buffer_leave_operation();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t capacity = seismic_buffer_capacity();

    size_t write_index;
    uint64_t frame_number;

    /*
     * This critical section only snapshots producer metadata.
     * Web/UI readers never hold it while copying waveform data.
     */
    SEISMIC_TEST_COUNT(metadata_critical_entries);
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    if (
        s_source == SEISMIC_SOURCE_NONE ||
        s_sample_rate_hz == 0
    ) {
        portEXIT_CRITICAL(
            &s_metadata_lock
        );

        seismic_buffer_leave_operation();
        return ESP_ERR_INVALID_STATE;
    }

    write_index =
        s_write_index;

    frame_number =
        s_total_frames + 1U;
    if (!has_source_sample_index) {
        source_sample_index = frame_number - 1U;
    }

    portEXIT_CRITICAL(
        &s_metadata_lock
    );

    seismic_buffer_write_slot(
        write_index,
        frame_number,
        source_sample_index,
        timestamp_us,
        channel_uv
    );

    /*
     * Publish ring metadata only after the slot is complete.
     */
    SEISMIC_TEST_COUNT(metadata_critical_entries);
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    s_write_index =
        write_index + 1U;

    if (s_write_index >= capacity) {
        s_write_index =
            0;
    }

    if (s_frame_count < capacity) {
        s_frame_count++;
    } else {
        s_dropped_frames_total = seismic_buffer_saturating_add_u64(
            s_dropped_frames_total, 1U
        );
    }
    if (s_frame_count > s_peak_frame_count) {
        s_peak_frame_count = s_frame_count;
    }

    s_total_frames =
        frame_number;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );

    seismic_buffer_leave_operation();
    return ESP_OK;
}


esp_err_t seismic_buffer_push(
    int64_t timestamp_us,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    return seismic_buffer_push_impl(timestamp_us, 0U, false, channel_uv);
}


esp_err_t seismic_buffer_push_indexed(
    int64_t timestamp_us,
    uint64_t source_sample_index,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    return seismic_buffer_push_impl(
        timestamp_us,
        source_sample_index,
        true,
        channel_uv
    );
}


esp_err_t seismic_buffer_copy_latest(
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    seismic_buffer_info_t *info
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        frames == NULL ||
        frame_count == NULL ||
        info == NULL ||
        max_frames == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_buffer_enter_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t capacity = seismic_buffer_capacity();
    if (s_frames == NULL || s_frame_numbers == NULL ||
        s_source_sample_indices == NULL ||
        s_sequences == NULL || capacity == 0U) {
        seismic_buffer_leave_operation();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;

    for (
        unsigned snapshot_attempt = 0;
        snapshot_attempt < SEISMIC_SNAPSHOT_RETRIES;
        ++snapshot_attempt
    ) {
        seismic_metadata_t metadata;

        seismic_buffer_snapshot_metadata(
            &metadata
        );

        size_t count =
            metadata.frame_count;

        if (count > max_frames) {
            count =
                max_frames;
        }

        seismic_buffer_publish_info(
            &metadata,
            info
        );

        if (count == 0) {
            *frame_count = 0U;
            result = ESP_OK;
            break;
        }

        const uint64_t first_frame_number =
            metadata.total_frames -
            (uint64_t)count +
            1U;

        bool consistent =
            true;

        for (
            size_t i = 0;
            i < count;
            ++i
        ) {
            const uint64_t expected_frame_number =
                first_frame_number +
                (uint64_t)i;

            const size_t source_index =
                (size_t)(
                    (expected_frame_number - 1U) %
                    capacity
                );

            if (
                !seismic_buffer_read_slot(
                    source_index,
                    expected_frame_number,
                    &frames[i],
                    NULL
                )
            ) {
                consistent =
                    false;

                break;
            }
        }

        if (consistent) {
            *frame_count = count;
            result = ESP_OK;
            break;
        }
        if (snapshot_attempt + 1U < SEISMIC_SNAPSHOT_RETRIES) {
            SEISMIC_TEST_COUNT(snapshot_retries);
        }
    }

    /*
     * A producer wrap raced every retry. This is transient and,
     * importantly, never stalls the acquisition task.
     */
    seismic_buffer_leave_operation();
    return result;
}


esp_err_t seismic_buffer_copy_since_with_source_indices(
    uint64_t *cursor,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *dropped_frames,
    seismic_buffer_info_t *info
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        cursor == NULL ||
        frames == NULL ||
        frame_count == NULL ||
        dropped_frames == NULL ||
        info == NULL ||
        max_frames == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_count = 0;
    *dropped_frames = 0;

    if (!seismic_buffer_enter_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t capacity = seismic_buffer_capacity();
    if (s_frames == NULL || s_frame_numbers == NULL ||
        s_source_sample_indices == NULL ||
        s_sequences == NULL || capacity == 0U) {
        seismic_buffer_leave_operation();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;

    for (
        unsigned snapshot_attempt = 0;
        snapshot_attempt < SEISMIC_SNAPSHOT_RETRIES;
        ++snapshot_attempt
    ) {
        seismic_metadata_t metadata;
        seismic_buffer_snapshot_metadata(&metadata);
        seismic_buffer_publish_info(&metadata, info);

        if (*cursor > metadata.total_frames) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        if (metadata.frame_count == 0 || *cursor == metadata.total_frames) {
            result = ESP_OK;
            break;
        }

        const uint64_t oldest_frame_number =
            metadata.total_frames -
            (uint64_t)metadata.frame_count +
            1U;

        uint64_t first_frame_number =
            *cursor + 1U;

        uint64_t dropped = 0;
        if (first_frame_number < oldest_frame_number) {
            dropped =
                oldest_frame_number -
                first_frame_number;
            first_frame_number =
                oldest_frame_number;
        }

        uint64_t available =
            metadata.total_frames -
            first_frame_number +
            1U;

        size_t count =
            available > (uint64_t)max_frames
                ? max_frames
                : (size_t)available;

        bool consistent = true;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t expected_frame_number =
                first_frame_number +
                (uint64_t)i;

            const size_t source_index =
                (size_t)(
                    (expected_frame_number - 1U) %
                    capacity
                );

            if (!seismic_buffer_read_slot(
                    source_index,
                    expected_frame_number,
                    &frames[i],
                    source_sample_indices != NULL
                        ? &source_sample_indices[i]
                        : NULL
                )) {
                consistent = false;
                break;
            }
        }

        if (!consistent) {
            if (snapshot_attempt + 1U < SEISMIC_SNAPSHOT_RETRIES) {
                SEISMIC_TEST_COUNT(snapshot_retries);
            }
            continue;
        }

        *dropped_frames = dropped;
        *frame_count = count;
        *cursor =
            first_frame_number +
            (uint64_t)count -
            1U;
        result = ESP_OK;
        break;
    }

    seismic_buffer_leave_operation();
    return result;
}


esp_err_t seismic_buffer_copy_since(
    uint64_t *cursor,
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *dropped_frames,
    seismic_buffer_info_t *info
)
{
    return seismic_buffer_copy_since_with_source_indices(
        cursor,
        frames,
        NULL,
        max_frames,
        frame_count,
        dropped_frames,
        info
    );
}


const char *seismic_buffer_source_name(
    seismic_source_t source
)
{
    switch (source) {
        case SEISMIC_SOURCE_TEST:
            return "TEST";

        case SEISMIC_SOURCE_AD7606:
            return "AD7606";

        case SEISMIC_SOURCE_NONE:
        default:
            return "NONE";
    }
}
