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

typedef enum {
    INJECT_BEFORE_ACQUIRE = 0,
    INJECT_AFTER_RELEASE,
} injection_point_t;

typedef enum {
    RESPONSE_STATUS = 0,
    RESPONSE_BLOCK,
} response_kind_t;

typedef struct {
    nrf_log_ring_t *ring;
    unsigned int depth;
    unsigned int lock_calls;
    unsigned int unlock_calls;
    nrf_log_lock_key_t next_key;
    nrf_log_lock_key_t active_key;
    injection_point_t injection_point;
    uint8_t injection_armed;
    uint8_t injection_fired;
    uint8_t reenter_on_generation;
    uint32_t uptime_ms;
    uint32_t values;
    uint32_t next_boot_id;
} harness_t;

typedef struct {
    nrf_log_ring_t ring;
    nrf_log_lock_interface_t lock;
    harness_t harness;
} fixture_t;

static void append_next(harness_t *harness);

static nrf_log_lock_key_t harness_lock(void *context)
{
    harness_t *harness = (harness_t *)context;
    nrf_log_lock_key_t key;

    ++harness->lock_calls;
    if (harness->injection_armed != 0U &&
        harness->injection_point == INJECT_BEFORE_ACQUIRE &&
        harness->injection_fired == 0U) {
        harness->injection_fired = 1U;
        append_next(harness);
    }
    CHECK(harness->depth == 0U);
    harness->depth = 1U;
    key = ++harness->next_key;
    harness->active_key = key;
    return key;
}

static void harness_unlock(void *context, nrf_log_lock_key_t key)
{
    harness_t *harness = (harness_t *)context;

    CHECK(harness->depth == 1U);
    CHECK(key == harness->active_key);
    harness->depth = 0U;
    ++harness->unlock_calls;
    if (harness->injection_armed != 0U &&
        harness->injection_point == INJECT_AFTER_RELEASE &&
        harness->injection_fired == 0U) {
        harness->injection_fired = 1U;
        append_next(harness);
    }
}

static uint32_t next_boot_id(void *context)
{
    harness_t *harness = (harness_t *)context;

    if (harness->reenter_on_generation != 0U) {
        harness->reenter_on_generation = 0U;
        CHECK(nrf_log_ring_append(
            harness->ring,
            NRF_LOG_SUBSYSTEM_LOG,
            NRF_LOG_SEVERITY_INFO,
            NRF_LOG_EVENT_LOG_TEST_EVENT,
            0xBAD0308U,
            ~0xBAD0308U,
            harness->uptime_ms) == NRF_LOG_ERROR_REENTRANT_LOSS);
    }
    return harness->next_boot_id;
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

static void append_next(harness_t *harness)
{
    const uint32_t value = harness->values++ ^ active_seed;

    CHECK(nrf_log_ring_append(
        harness->ring,
        NRF_LOG_SUBSYSTEM_LOG,
        NRF_LOG_SEVERITY_INFO,
        NRF_LOG_EVENT_LOG_TEST_EVENT,
        value,
        ~value,
        harness->uptime_ms++) == NRF_LOG_ERROR_OK);
}

static void initialize(fixture_t *fixture, uint32_t seed)
{
    const uint32_t boot_id = 0x30800000U ^ seed;

    memset(fixture, 0, sizeof(*fixture));
    fixture->harness.ring = &fixture->ring;
    fixture->harness.next_boot_id = boot_id + 1U;
    fixture->harness.uptime_ms = 1U;
    fixture->lock.lock = harness_lock;
    fixture->lock.unlock = harness_unlock;
    fixture->lock.context = &fixture->harness;
    CHECK(nrf_log_ring_init(
        &fixture->ring,
        boot_id,
        0U,
        &fixture->lock,
        next_boot_id,
        &fixture->harness) == NRF_LOG_ERROR_OK);
}

static nrf_log_status_t status_of(fixture_t *fixture)
{
    nrf_log_status_t status;

    CHECK(nrf_log_ring_status_snapshot(&fixture->ring, &status) == NRF_LOG_ERROR_OK);
    CHECK(status.available <= status.capacity);
    CHECK(status.next_seq - status.oldest_seq == status.available);
    return status;
}

static void assert_status_equal(const nrf_log_status_t *actual, const nrf_log_status_t *expected)
{
    CHECK(actual->flags == expected->flags);
    CHECK(actual->capacity == expected->capacity);
    CHECK(actual->available == expected->available);
    CHECK(actual->boot_id == expected->boot_id);
    CHECK(actual->oldest_seq == expected->oldest_seq);
    CHECK(actual->next_seq == expected->next_seq);
    CHECK(actual->overwritten_total == expected->overwritten_total);
    CHECK(actual->lost_internal_total == expected->lost_internal_total);
    CHECK(actual->next_seq - actual->oldest_seq == actual->available);
}

static void assert_records_integral(const nrf_log_block_t *block)
{
    uint8_t index;

    CHECK(block->next_cursor == block->first_seq + block->record_count);
    for (index = 0U; index < block->record_count; ++index) {
        const nrf_log_record_t *record = &block->records[index];

        CHECK(record->seq == block->first_seq + index);
        if (record->event_id == NRF_LOG_EVENT_LOG_TEST_EVENT) {
            CHECK(record->arg1 == ~record->arg0);
        } else {
            CHECK(record->event_id == NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER);
            CHECK(record->seq == 0U);
            CHECK(record->arg0 != record->arg1);
            CHECK(record->arg1 == block->boot_id);
        }
    }
}

static void assert_block_matches(
    const uint8_t *frame,
    size_t frame_length,
    const nrf_log_read_request_t *request,
    const nrf_log_status_t *expected)
{
    nrf_log_block_t block;
    uint32_t start_seq;
    uint32_t remaining;
    uint8_t record_count;

    CHECK(nrf_log_protocol_decode_block(frame, frame_length, NULL, &block) == NRF_LOG_ERROR_OK);
    CHECK(block.request_id == request->request_id);
    CHECK(block.boot_id == expected->boot_id);
    CHECK(block.oldest_seq == expected->oldest_seq);
    CHECK(block.overwritten_total == expected->overwritten_total);
    if (request->expected_boot_id != expected->boot_id) {
        CHECK((block.flags & NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH) != 0U);
        start_seq = expected->next_seq;
    } else if (request->cursor_seq < expected->oldest_seq) {
        CHECK((block.flags & NRF_LOG_BLOCK_FLAG_GAP) != 0U);
        start_seq = expected->oldest_seq;
    } else {
        start_seq = request->cursor_seq;
    }
    remaining = expected->next_seq - start_seq;
    record_count = (uint8_t)(remaining < request->max_records
        ? remaining
        : request->max_records);
    CHECK(block.first_seq == start_seq);
    CHECK(block.record_count == record_count);
    assert_records_integral(&block);
}

static void run_interleave_case(
    uint32_t seed,
    uint16_t initial_count,
    uint8_t rollover,
    response_kind_t response_kind,
    injection_point_t injection_point)
{
    fixture_t fixture;
    nrf_log_status_t before;
    nrf_log_status_t after_boundary;
    nrf_log_status_t expected;
    nrf_log_status_t final_status;
    uint32_t random_state = seed;
    uint8_t first[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    uint8_t retry[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t first_length = 0U;
    size_t retry_length = 0U;
    uint32_t request_id = seed ^ ((uint32_t)initial_count << 8U) ^
        ((uint32_t)response_kind << 4U) ^ (uint32_t)injection_point;
    nrf_log_read_request_t read_request;
    uint16_t index;

    active_seed = seed;
    initialize(&fixture, seed);
    for (index = 0U; index < initial_count; ++index) {
        fixture.harness.values = random_next(&random_state);
        append_next(&fixture.harness);
    }
    if (rollover != 0U) {
        CHECK(initial_count == 0U);
        CHECK(nrf_log_ring_test_set_next_seq(
            &fixture.ring,
            NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
    }
    before = status_of(&fixture);
    memset(&read_request, 0, sizeof(read_request));
    read_request.request_id = request_id;
    read_request.expected_boot_id = before.boot_id;
    read_request.cursor_seq = before.oldest_seq;
    read_request.max_records = NRF_LOG_PROTOCOL_MAX_RECORDS;

    fixture.harness.injection_point = injection_point;
    fixture.harness.injection_armed = 1U;
    if (response_kind == RESPONSE_STATUS) {
        CHECK(nrf_log_ring_build_status_frame(
            &fixture.ring,
            request_id,
            first,
            sizeof(first),
            &first_length) == NRF_LOG_ERROR_OK);
    } else {
        CHECK(nrf_log_ring_build_block_frame(
            &fixture.ring,
            read_request.request_id,
            read_request.expected_boot_id,
            read_request.cursor_seq,
            read_request.max_records,
            first,
            sizeof(first),
            &first_length) == NRF_LOG_ERROR_OK);
    }
    CHECK(fixture.harness.injection_fired != 0U);
    CHECK(fixture.harness.depth == 0U);
    CHECK(fixture.harness.lock_calls == fixture.harness.unlock_calls);
    after_boundary = status_of(&fixture);
    expected = injection_point == INJECT_BEFORE_ACQUIRE ? after_boundary : before;

    if (response_kind == RESPONSE_STATUS) {
        nrf_log_status_t decoded;
        CHECK(nrf_log_protocol_decode_status(first, first_length, &decoded) == NRF_LOG_ERROR_OK);
        CHECK(decoded.request_id == request_id);
        assert_status_equal(&decoded, &expected);
    } else {
        assert_block_matches(first, first_length, &read_request, &expected);
    }

    /* A later append changes live state while the cached request must remain byte-identical. */
    append_next(&fixture.harness);
    if (response_kind == RESPONSE_STATUS) {
        CHECK(nrf_log_ring_build_status_frame(
            &fixture.ring,
            request_id,
            retry,
            sizeof(retry),
            &retry_length) == NRF_LOG_ERROR_OK);
    } else {
        CHECK(nrf_log_ring_build_block_frame(
            &fixture.ring,
            read_request.request_id,
            read_request.expected_boot_id,
            read_request.cursor_seq,
            read_request.max_records,
            retry,
            sizeof(retry),
            &retry_length) == NRF_LOG_ERROR_OK);
    }
    CHECK(first_length == retry_length);
    CHECK(memcmp(first, retry, first_length) == 0);
    final_status = status_of(&fixture);
    CHECK(final_status.next_seq != expected.next_seq || final_status.boot_id != expected.boot_id);
    CHECK(final_status.available <= final_status.capacity);
    CHECK(final_status.next_seq - final_status.oldest_seq == final_status.available);
    CHECK(final_status.overwritten_total >= expected.overwritten_total);
    CHECK(final_status.lost_internal_total == expected.lost_internal_total);

    if (rollover != 0U) {
        nrf_log_block_t block;
        uint8_t generation_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        size_t generation_length = 0U;
        const nrf_log_read_request_t generation_request = {
            .request_id = request_id + 1U,
            .expected_boot_id = final_status.boot_id,
            .cursor_seq = final_status.oldest_seq,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        CHECK(nrf_log_ring_build_block_frame(
            &fixture.ring,
            generation_request.request_id,
            generation_request.expected_boot_id,
            generation_request.cursor_seq,
            generation_request.max_records,
            generation_frame,
            sizeof(generation_frame),
            &generation_length) == NRF_LOG_ERROR_OK);
        CHECK(nrf_log_protocol_decode_block(
            generation_frame,
            generation_length,
            NULL,
            &block) == NRF_LOG_ERROR_OK);
        CHECK(block.record_count == 3U);
        CHECK(block.records[0].seq == 0U);
        CHECK(block.records[0].event_id == NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER);
        assert_records_integral(&block);
    }
}

static void test_all_ring_boundaries_and_lock_points(void)
{
    static const uint16_t initial_counts[] = {
        0U,
        1U,
        NRF_LOG_RING_CAPACITY - 1U,
        NRF_LOG_RING_CAPACITY,
        NRF_LOG_RING_CAPACITY + 1U,
    };
    static const uint32_t seeds[] = {0x308C0FFEU, 0x1EAFB00CU, 0x5EED0308U};
    size_t count_index;

    for (count_index = 0U;
         count_index < sizeof(initial_counts) / sizeof(initial_counts[0]);
         ++count_index) {
        const size_t seed_count = initial_counts[count_index] >= NRF_LOG_RING_CAPACITY - 1U
            ? sizeof(seeds) / sizeof(seeds[0])
            : 1U;
        size_t seed_index;
        for (seed_index = 0U; seed_index < seed_count; ++seed_index) {
            response_kind_t response_kind;
            for (response_kind = RESPONSE_STATUS; response_kind <= RESPONSE_BLOCK;
                 response_kind = (response_kind_t)(response_kind + 1)) {
                injection_point_t injection_point;
                for (injection_point = INJECT_BEFORE_ACQUIRE;
                     injection_point <= INJECT_AFTER_RELEASE;
                     injection_point = (injection_point_t)(injection_point + 1)) {
                    run_interleave_case(
                        seeds[seed_index],
                        initial_counts[count_index],
                        0U,
                        response_kind,
                        injection_point);
                }
            }
        }
    }
}

static void test_rollover_and_reentrant_loss(void)
{
    response_kind_t response_kind;
    injection_point_t injection_point;

    for (response_kind = RESPONSE_STATUS; response_kind <= RESPONSE_BLOCK;
         response_kind = (response_kind_t)(response_kind + 1)) {
        for (injection_point = INJECT_BEFORE_ACQUIRE;
             injection_point <= INJECT_AFTER_RELEASE;
             injection_point = (injection_point_t)(injection_point + 1)) {
            run_interleave_case(
                0x308C0FFEU,
                0U,
                1U,
                response_kind,
                injection_point);
        }
    }
    {
        fixture_t fixture;
        nrf_log_status_t status;
        uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
        size_t frame_length = 0U;
        nrf_log_status_t decoded;

        /* Frozen contract: nrf_log_protocol.h names REENTRANT_LOSS, and
         * nrf_log_ring.c returns it while saturating lost_internal_total when busy. */
        active_seed = 0x308C0FFEU;
        initialize(&fixture, active_seed);
        CHECK(nrf_log_ring_test_set_next_seq(
            &fixture.ring,
            NRF_LOG_RING_ROLLOVER_SEQ) == NRF_LOG_ERROR_OK);
        fixture.harness.reenter_on_generation = 1U;
        append_next(&fixture.harness);
        status = status_of(&fixture);
        CHECK(status.lost_internal_total == 1U);
        CHECK(status.available == 2U);
        CHECK(status.oldest_seq == 0U && status.next_seq == 2U);
        CHECK(status.next_seq - status.oldest_seq == status.available);
        CHECK(nrf_log_ring_build_status_frame(
            &fixture.ring,
            0x30810U,
            frame,
            sizeof(frame),
            &frame_length) == NRF_LOG_ERROR_OK);
        CHECK(nrf_log_protocol_decode_status(frame, frame_length, &decoded) == NRF_LOG_ERROR_OK);
        CHECK(decoded.lost_internal_total == 1U);
        CHECK(decoded.next_seq - decoded.oldest_seq == decoded.available);
    }
}

int main(void)
{
    test_all_ring_boundaries_and_lock_points();
    test_rollover_and_reentrant_loss();
    printf("nrf_log_interleave_test: PASS (%u checks)\n", checks);
    return 0;
}
