#include "nrf_log_events.h"
#include "nrf_log_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t active_seed = 0x308C0FFEU;
static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "seed=0x%08lX failed at %s:%d: %s\n", \
            (unsigned long)active_seed, __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct {
    unsigned int depth;
    unsigned int lock_calls;
    unsigned int unlock_calls;
} fake_lock_t;

typedef struct {
    nrf_log_ring_t *ring;
    uint32_t next_boot_id;
    unsigned int reentrant_appends;
    uint8_t return_zero;
} boot_source_t;

typedef struct {
    nrf_log_ring_t ring;
    fake_lock_t lock_state;
    boot_source_t boot_source;
    nrf_log_lock_interface_t lock;
} fixture_t;

static nrf_log_lock_key_t fake_lock(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    CHECK(lock->depth == 0U);
    lock->depth = 1U;
    ++lock->lock_calls;
    return lock->lock_calls;
}

static void fake_unlock(void *context, nrf_log_lock_key_t key)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    CHECK(lock->depth == 1U);
    CHECK(key == lock->lock_calls);
    lock->depth = 0U;
    ++lock->unlock_calls;
}

static uint32_t next_boot_id(void *context)
{
    boot_source_t *source = (boot_source_t *)context;
    unsigned int i;
    for (i = 0U; i < source->reentrant_appends; ++i) {
        CHECK(nrf_log_ring_append(
            source->ring,
            NRF_LOG_SUBSYSTEM_LOG,
            NRF_LOG_SEVERITY_INFO,
            NRF_LOG_EVENT_LOG_TEST_EVENT,
            i,
            0U,
            123U) == NRF_LOG_ERROR_REENTRANT_LOSS);
    }
    return source->return_zero ? 0U : source->next_boot_id;
}

static void initialize(fixture_t *fixture, uint32_t boot_id, uint8_t fallback)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->lock.lock = fake_lock;
    fixture->lock.unlock = fake_unlock;
    fixture->lock.context = &fixture->lock_state;
    fixture->boot_source.ring = &fixture->ring;
    fixture->boot_source.next_boot_id = boot_id + 1U;
    CHECK(nrf_log_ring_init(
        &fixture->ring,
        boot_id,
        fallback,
        &fixture->lock,
        next_boot_id,
        &fixture->boot_source) == NRF_LOG_ERROR_OK);
}

static void append_value(fixture_t *fixture, uint32_t value, uint32_t uptime_ms)
{
    CHECK(nrf_log_ring_append(
        &fixture->ring,
        NRF_LOG_SUBSYSTEM_LOG,
        NRF_LOG_SEVERITY_INFO,
        NRF_LOG_EVENT_LOG_TEST_EVENT,
        value,
        ~value,
        uptime_ms) == NRF_LOG_ERROR_OK);
}

static nrf_log_status_t status_of(fixture_t *fixture)
{
    nrf_log_status_t status;
    CHECK(nrf_log_ring_status_snapshot(&fixture->ring, &status) == NRF_LOG_ERROR_OK);
    CHECK(status.available <= status.capacity);
    CHECK(status.next_seq >= status.oldest_seq);
    CHECK(status.next_seq - status.oldest_seq == status.available);
    return status;
}

static void test_capacity_overflow_and_wrap(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t frame_length;
    nrf_log_block_t block;
    nrf_log_block_validation_t validation;
    uint32_t i;
    initialize(&fixture, 0x1001U, 1U);
    for (i = 0U; i < NRF_LOG_RING_CAPACITY - 1U; ++i) {
        append_value(&fixture, i, i);
    }
    status = status_of(&fixture);
    CHECK(status.available == NRF_LOG_RING_CAPACITY - 1U);
    CHECK(status.oldest_seq == 0U && status.next_seq == NRF_LOG_RING_CAPACITY - 1U);
    CHECK(status.overwritten_total == 0U);
    append_value(&fixture, i, i);
    ++i;
    status = status_of(&fixture);
    CHECK(status.available == NRF_LOG_RING_CAPACITY);
    CHECK(status.oldest_seq == 0U && status.next_seq == NRF_LOG_RING_CAPACITY);
    append_value(&fixture, i, i);
    ++i;
    status = status_of(&fixture);
    CHECK(status.available == NRF_LOG_RING_CAPACITY);
    CHECK(status.oldest_seq == 1U && status.next_seq == NRF_LOG_RING_CAPACITY + 1U);
    CHECK(status.overwritten_total == 1U);

    for (i = 0U; i < 300U; ++i) {
        append_value(&fixture, i + 130U, i + 130U);
    }
    status = status_of(&fixture);
    CHECK(status.available == NRF_LOG_RING_CAPACITY);
    CHECK(status.oldest_seq == 301U && status.next_seq == 429U);
    CHECK(status.overwritten_total == 301U);
    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 10U, status.boot_id, 0U, 5U,
        frame, sizeof(frame), &frame_length) == NRF_LOG_ERROR_OK);
    memset(&validation, 0, sizeof(validation));
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = status.boot_id;
    validation.has_cursor = true;
    validation.cursor_seq = 0U;
    validation.has_max_records = true;
    validation.max_records = 5U;
    validation.has_status_next_seq = true;
    validation.status_next_seq = status.next_seq;
    CHECK(nrf_log_protocol_decode_block(frame, frame_length, &validation, &block) == NRF_LOG_ERROR_OK);
    CHECK((block.flags & NRF_LOG_BLOCK_FLAG_GAP) != 0U);
    CHECK((block.flags & NRF_LOG_BLOCK_FLAG_MORE) != 0U);
    CHECK(block.first_seq == status.oldest_seq && block.next_cursor == status.oldest_seq + 5U);
    CHECK(block.records[0].seq == status.oldest_seq);
    CHECK(fixture.lock_state.depth == 0U);
    CHECK(fixture.lock_state.lock_calls == fixture.lock_state.unlock_calls);
}

static void test_counter_saturation(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    uint32_t i;
    initialize(&fixture, 0x2001U, 0U);
    for (i = 0U; i < NRF_LOG_RING_CAPACITY; ++i) {
        append_value(&fixture, i, i);
    }
    CHECK(nrf_log_ring_test_set_totals(&fixture.ring, UINT32_MAX - 1U, UINT32_MAX - 1U) == NRF_LOG_ERROR_OK);
    append_value(&fixture, i, i);
    ++i;
    append_value(&fixture, i, i);
    status = status_of(&fixture);
    CHECK(status.overwritten_total == UINT32_MAX);
    CHECK(status.lost_internal_total == UINT32_MAX - 1U);
}

static void test_generation_rollover_and_event(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    nrf_log_block_t block;
    nrf_log_block_validation_t validation;
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t frame_length;

    initialize(&fixture, 1U, 1U);
    fixture.boot_source.next_boot_id = 2U;
    CHECK(nrf_log_ring_test_set_next_seq(&fixture.ring, NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
    append_value(&fixture, 0xABCDEF01U, 100U);
    status = status_of(&fixture);
    CHECK(status.boot_id == 2U);
    CHECK(status.oldest_seq == 0U && status.next_seq == 2U && status.available == 2U);
    CHECK(status.overwritten_total == 0U);
    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 20U, 2U, 0U, 5U, frame, sizeof(frame), &frame_length) == NRF_LOG_ERROR_OK);
    memset(&validation, 0, sizeof(validation));
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = 2U;
    validation.has_cursor = true;
    validation.cursor_seq = 0U;
    validation.has_max_records = true;
    validation.max_records = 5U;
    validation.has_status_next_seq = true;
    validation.status_next_seq = status.next_seq;
    CHECK(nrf_log_protocol_decode_block(frame, frame_length, &validation, &block) == NRF_LOG_ERROR_OK);
    CHECK(block.records[0].seq == 0U && block.records[0].event_id == NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER);
    CHECK(block.records[0].subsystem == NRF_LOG_SUBSYSTEM_LOG);
    CHECK(block.records[0].severity == NRF_LOG_SEVERITY_WARN);
    CHECK(block.records[0].arg0 == 1U && block.records[0].arg1 == 2U);
    CHECK(block.records[1].seq == 1U && block.records[1].arg0 == 0xABCDEF01U);
    CHECK(fixture.boot_source.next_boot_id == 2U);

    initialize(&fixture, 7U, 0U);
    fixture.boot_source.return_zero = 1U;
    CHECK(nrf_log_ring_test_set_next_seq(&fixture.ring, NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
    CHECK(nrf_log_ring_append(
        &fixture.ring, NRF_LOG_SUBSYSTEM_LOG, NRF_LOG_SEVERITY_INFO,
        NRF_LOG_EVENT_LOG_TEST_EVENT, 1U, 0U, 9U) == NRF_LOG_ERROR_BAD_BOOT_ID);
    status = status_of(&fixture);
    CHECK(status.boot_id == 7U && status.next_seq == NRF_LOG_RING_ROLLOVER_SEQ);
}

static void test_idempotent_response_cache(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    nrf_log_status_t decoded_status;
    uint8_t first[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    uint8_t retry[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    uint8_t changed[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t first_length;
    size_t retry_length;
    size_t changed_length;
    nrf_log_block_t block;
    nrf_log_block_validation_t validation;

    initialize(&fixture, 0x3001U, 0U);
    CHECK(nrf_log_ring_build_status_frame(
        &fixture.ring, 44U, first, sizeof(first), &first_length) == NRF_LOG_ERROR_OK);
    append_value(&fixture, 1U, 1U);
    CHECK(nrf_log_ring_build_status_frame(
        &fixture.ring, 44U, retry, sizeof(retry), &retry_length) == NRF_LOG_ERROR_OK);
    CHECK(first_length == retry_length && memcmp(first, retry, first_length) == 0);
    CHECK(nrf_log_ring_build_status_frame(
        &fixture.ring, 45U, changed, sizeof(changed), &changed_length) == NRF_LOG_ERROR_OK);
    CHECK(memcmp(first, changed, first_length) != 0);
    CHECK(nrf_log_protocol_decode_status(changed, changed_length, &decoded_status) == NRF_LOG_ERROR_OK);
    CHECK(decoded_status.available == 1U && decoded_status.request_id == 45U);

    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 55U, 0x3001U, 0U, 5U, first, sizeof(first), &first_length) == NRF_LOG_ERROR_OK);
    append_value(&fixture, 2U, 2U);
    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 55U, 0x3001U, 0U, 5U, retry, sizeof(retry), &retry_length) == NRF_LOG_ERROR_OK);
    CHECK(first_length == retry_length && memcmp(first, retry, first_length) == 0);
    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 55U, 0x3001U, 1U, 5U, changed, sizeof(changed), &changed_length) == NRF_LOG_ERROR_OK);
    CHECK(memcmp(first, changed, first_length) != 0);
    status = status_of(&fixture);
    memset(&validation, 0, sizeof(validation));
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = status.boot_id;
    validation.has_cursor = true;
    validation.cursor_seq = 1U;
    validation.has_max_records = true;
    validation.max_records = 5U;
    validation.has_status_next_seq = true;
    validation.status_next_seq = status.next_seq;
    CHECK(nrf_log_protocol_decode_block(changed, changed_length, &validation, &block) == NRF_LOG_ERROR_OK);
    CHECK(block.record_count == 1U && block.records[0].seq == 1U);
}

static void test_boot_mismatch_and_uptime_rules(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    nrf_log_block_t block;
    nrf_log_block_validation_t validation;
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t frame_length;
    initialize(&fixture, 0x4001U, 0U);
    append_value(&fixture, 1U, 0xF1000001U);
    append_value(&fixture, 2U, 0x00000001U);
    CHECK(nrf_log_ring_append(
        &fixture.ring, NRF_LOG_SUBSYSTEM_LOG, NRF_LOG_SEVERITY_INFO,
        NRF_LOG_EVENT_LOG_TEST_EVENT, 3U, 0U, 0U) == NRF_LOG_ERROR_UPTIME_REGRESSION);
    status = status_of(&fixture);
    CHECK(status.available == 2U && status.next_seq == 2U);
    CHECK(nrf_log_ring_build_block_frame(
        &fixture.ring, 66U, 0xDEADBEEFU, 0U, 5U,
        frame, sizeof(frame), &frame_length) == NRF_LOG_ERROR_OK);
    memset(&validation, 0, sizeof(validation));
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = 0xDEADBEEFU;
    validation.has_cursor = true;
    validation.cursor_seq = 0U;
    validation.has_max_records = true;
    validation.max_records = 5U;
    validation.has_status_next_seq = true;
    validation.status_next_seq = status.next_seq;
    CHECK(nrf_log_protocol_decode_block(frame, frame_length, &validation, &block) == NRF_LOG_ERROR_OK);
    CHECK(block.record_count == 0U && block.first_seq == status.next_seq);
    CHECK((block.flags & NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH) != 0U);
}

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void test_seeded_ring_properties(void)
{
    static const uint32_t seeds[] = {0x00000001U, 0x12345678U, 0xC0FFEE01U, 0x308C0FFEU};
    size_t seed_index;
    for (seed_index = 0U; seed_index < sizeof(seeds) / sizeof(seeds[0]); ++seed_index) {
        fixture_t fixture;
        uint32_t state = seeds[seed_index];
        uint32_t boot_id = 0x5000U + (uint32_t)seed_index * 0x100U;
        uint32_t uptime_ms = 0U;
        unsigned int step;
        active_seed = state;
        initialize(&fixture, boot_id, (uint8_t)(seed_index & 1U));
        for (step = 0U; step < 10000U; ++step) {
            uint32_t operation = random_next(&state) % 100U;
            active_seed = state;
            if (operation < 62U) {
                uint32_t value = random_next(&state);
                uptime_ms += random_next(&state) % 4U;
                CHECK(nrf_log_ring_append(
                    &fixture.ring,
                    (uint8_t)(NRF_LOG_SUBSYSTEM_BOOT + random_next(&state) % 9U),
                    (uint8_t)(random_next(&state) % 5U),
                    NRF_LOG_EVENT_LOG_TEST_EVENT,
                    value,
                    ~value,
                    uptime_ms) == NRF_LOG_ERROR_OK);
            } else if (operation < 84U) {
                nrf_log_status_t status = status_of(&fixture);
                nrf_log_block_t block;
                nrf_log_block_validation_t validation;
                uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
                size_t frame_length;
                uint8_t max_records = (uint8_t)(1U + random_next(&state) % NRF_LOG_PROTOCOL_MAX_RECORDS);
                uint32_t cursor;
                uint32_t selector = random_next(&state);
                if ((selector & 3U) == 0U && status.oldest_seq != 0U) {
                    cursor = status.oldest_seq - 1U;
                } else if (status.available == 0U) {
                    cursor = status.next_seq;
                } else {
                    cursor = status.oldest_seq + selector % (status.available + 1U);
                }
                CHECK(nrf_log_ring_build_block_frame(
                    &fixture.ring, step, status.boot_id, cursor, max_records,
                    frame, sizeof(frame), &frame_length) == NRF_LOG_ERROR_OK);
                memset(&validation, 0, sizeof(validation));
                validation.has_expected_boot_id = true;
                validation.expected_boot_id = status.boot_id;
                validation.has_cursor = true;
                validation.cursor_seq = cursor;
                validation.has_max_records = true;
                validation.max_records = max_records;
                validation.has_status_next_seq = true;
                validation.status_next_seq = status.next_seq;
                CHECK(nrf_log_protocol_decode_block(frame, frame_length, &validation, &block) == NRF_LOG_ERROR_OK);
                CHECK(block.record_count <= max_records);
                CHECK(block.oldest_seq == status.oldest_seq);
                CHECK(block.next_cursor == block.first_seq + block.record_count);
                if (cursor < status.oldest_seq) {
                    CHECK((block.flags & NRF_LOG_BLOCK_FLAG_GAP) != 0U);
                }
            } else {
                ++boot_id;
                initialize(&fixture, boot_id, (uint8_t)(seed_index & 1U));
                uptime_ms = 0U;
            }
            {
                nrf_log_status_t status = status_of(&fixture);
                CHECK(status.available <= NRF_LOG_RING_CAPACITY);
                CHECK(status.next_seq - status.oldest_seq == status.available);
            }
        }
    }
    active_seed = 0x308C0FFEU;
}

static void test_nonrecursive_loss_and_counter_saturation(void)
{
    fixture_t fixture;
    nrf_log_status_t status;
    unsigned int lock_calls;
    initialize(&fixture, 0x6001U, 0U);
    fixture.boot_source.next_boot_id = 0x6002U;
    fixture.boot_source.reentrant_appends = 2U;
    CHECK(nrf_log_ring_test_set_totals(&fixture.ring, 0U, UINT32_MAX - 1U) == NRF_LOG_ERROR_OK);
    CHECK(nrf_log_ring_test_set_next_seq(&fixture.ring, NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
    lock_calls = fixture.lock_state.lock_calls;
    append_value(&fixture, 9U, 50U);
    CHECK(fixture.lock_state.lock_calls == lock_calls + 1U);
    status = status_of(&fixture);
    CHECK(status.lost_internal_total == UINT32_MAX);
    CHECK(status.available == 2U && status.boot_id == 0x6002U);
}

int main(void)
{
    test_capacity_overflow_and_wrap();
    test_counter_saturation();
    test_generation_rollover_and_event();
    test_idempotent_response_cache();
    test_boot_mismatch_and_uptime_rules();
    test_seeded_ring_properties();
    test_nonrecursive_loss_and_counter_saturation();
    printf("nrf_log_ring_test: PASS (%u checks across 4 x 10000 property steps)\n", checks);
    return 0;
}
