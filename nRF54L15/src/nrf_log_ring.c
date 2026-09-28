#include "nrf_log_ring.h"

#include "nrf_log_events.h"

#include <string.h>

typedef struct {
    uint8_t request_type;
    uint8_t max_records;
    uint32_t request_id;
    uint32_t expected_boot_id;
    uint32_t cursor_seq;
} nrf_log_cache_key_t;

static nrf_log_lock_key_t lock_ring(nrf_log_ring_t *ring)
{
    nrf_log_lock_key_t key = ring->lock.lock(ring->lock.context);
    ring->busy = 1U;
    return key;
}

static void unlock_ring(nrf_log_ring_t *ring, nrf_log_lock_key_t key)
{
    ring->busy = 0U;
    ring->lock.unlock(ring->lock.context, key);
}

static void increment_lost_without_lock(nrf_log_ring_t *ring)
{
    /* ponytail: the per-ring busy flag treats overlapping arrivals as lost; add owner tracking only if T1 needs it. */
    if (ring->lost_internal_total != UINT32_MAX) {
        ++ring->lost_internal_total;
    }
}

static void saturating_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static int uptime_is_monotonic(uint32_t previous, uint32_t current)
{
    return current >= previous ||
        (previous > 0xF0000000U && current < 0x10000000U);
}

static void snapshot_locked(const nrf_log_ring_t *ring, uint32_t request_id, nrf_log_status_t *status)
{
    status->request_id = request_id;
    status->flags = (uint8_t)(NRF_LOG_STATUS_FLAG_ENABLED |
        (ring->boot_id_fallback ? NRF_LOG_STATUS_FLAG_BOOT_ID_FALLBACK : 0U));
    status->capacity = NRF_LOG_RING_CAPACITY;
    status->available = ring->count;
    status->boot_id = ring->boot_id;
    status->oldest_seq = ring->oldest_seq;
    status->next_seq = ring->next_seq;
    status->overwritten_total = ring->overwritten_total;
    status->lost_internal_total = ring->lost_internal_total;
}

static int cache_key_matches(const nrf_log_ring_t *ring, const nrf_log_cache_key_t *key)
{
    return ring->cache_valid != 0U &&
        ring->cache_request_type == key->request_type &&
        ring->cache_request_id == key->request_id &&
        ring->cache_expected_boot_id == key->expected_boot_id &&
        ring->cache_cursor_seq == key->cursor_seq &&
        ring->cache_max_records == key->max_records;
}

static nrf_log_error_t return_cached(
    const nrf_log_ring_t *ring,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length)
{
    if (frame_capacity < ring->cache_length) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    memcpy(frame, ring->cache_frame, ring->cache_length);
    *frame_length = ring->cache_length;
    return NRF_LOG_ERROR_OK;
}

static void save_cache(
    nrf_log_ring_t *ring,
    const nrf_log_cache_key_t *key,
    const uint8_t *frame,
    size_t frame_length)
{
    ring->cache_request_type = key->request_type;
    ring->cache_request_id = key->request_id;
    ring->cache_expected_boot_id = key->expected_boot_id;
    ring->cache_cursor_seq = key->cursor_seq;
    ring->cache_max_records = key->max_records;
    ring->cache_length = (uint8_t)frame_length;
    memcpy(ring->cache_frame, frame, frame_length);
    ring->cache_valid = 1U;
}

static uint8_t oldest_index(const nrf_log_ring_t *ring)
{
    return (uint8_t)((ring->head + NRF_LOG_RING_CAPACITY - ring->count) % NRF_LOG_RING_CAPACITY);
}

static nrf_log_error_t write_record_locked(nrf_log_ring_t *ring, const nrf_log_record_t *record)
{
    nrf_log_error_t result = nrf_log_protocol_encode_record(record, ring->records[ring->head]);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    if (ring->count == NRF_LOG_RING_CAPACITY) {
        ring->oldest_seq++;
        saturating_increment(&ring->overwritten_total);
    } else {
        if (ring->count == 0U) {
            ring->oldest_seq = record->seq;
        }
        ++ring->count;
    }
    ring->head = (uint8_t)((ring->head + 1U) % NRF_LOG_RING_CAPACITY);
    ring->next_seq = record->seq + 1U;
    return NRF_LOG_ERROR_OK;
}

static nrf_log_error_t last_uptime_locked(const nrf_log_ring_t *ring, uint32_t uptime_ms)
{
    uint8_t index;
    nrf_log_record_t last;
    if (ring->count == 0U) {
        return NRF_LOG_ERROR_OK;
    }
    index = (uint8_t)((ring->head + NRF_LOG_RING_CAPACITY - 1U) % NRF_LOG_RING_CAPACITY);
    if (nrf_log_protocol_decode_record(ring->records[index], &last) != NRF_LOG_ERROR_OK) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    return uptime_is_monotonic(last.uptime_ms, uptime_ms)
        ? NRF_LOG_ERROR_OK
        : NRF_LOG_ERROR_UPTIME_REGRESSION;
}

static nrf_log_error_t start_generation_locked(nrf_log_ring_t *ring, uint32_t uptime_ms)
{
    uint32_t previous_boot_id = ring->boot_id;
    uint32_t next_boot_id = ring->new_boot_id(ring->new_boot_id_context);
    const nrf_log_event_definition_t *event = nrf_log_event_find(NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER);
    nrf_log_record_t rollover;

    if (next_boot_id == 0U || next_boot_id == previous_boot_id || event == NULL) {
        return NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    ring->boot_id = next_boot_id;
    ring->head = 0U;
    ring->count = 0U;
    ring->oldest_seq = 0U;
    ring->next_seq = 0U;
    rollover.seq = 0U;
    rollover.uptime_ms = uptime_ms;
    rollover.event_id = NRF_LOG_EVENT_LOG_GENERATION_ROLLOVER;
    rollover.severity = event->default_severity;
    rollover.subsystem = event->subsystem;
    rollover.arg0 = previous_boot_id;
    rollover.arg1 = next_boot_id;
    return write_record_locked(ring, &rollover);
}

nrf_log_error_t nrf_log_ring_init(
    nrf_log_ring_t *ring,
    uint32_t boot_id,
    uint8_t boot_id_fallback,
    const nrf_log_lock_interface_t *lock,
    nrf_log_new_boot_id_fn new_boot_id,
    void *new_boot_id_context)
{
    if (ring == NULL || lock == NULL || lock->lock == NULL || lock->unlock == NULL ||
        new_boot_id == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (boot_id == 0U) {
        return NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    if (boot_id_fallback > 1U) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    memset(ring, 0, sizeof(*ring));
    ring->boot_id = boot_id;
    ring->boot_id_fallback = boot_id_fallback;
    ring->lock = *lock;
    ring->new_boot_id = new_boot_id;
    ring->new_boot_id_context = new_boot_id_context;
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_ring_append(
    nrf_log_ring_t *ring,
    uint8_t subsystem,
    uint8_t severity,
    uint16_t event_id,
    uint32_t arg0,
    uint32_t arg1,
    uint32_t uptime_ms)
{
    nrf_log_lock_key_t key;
    nrf_log_record_t record;
    nrf_log_error_t result;
    uint8_t serialized[NRF_LOG_PROTOCOL_RECORD_LENGTH];

    if (ring == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (ring->busy != 0U) {
        increment_lost_without_lock(ring);
        return NRF_LOG_ERROR_REENTRANT_LOSS;
    }
    record.seq = 0U;
    record.uptime_ms = uptime_ms;
    record.event_id = event_id;
    record.severity = severity;
    record.subsystem = subsystem;
    record.arg0 = arg0;
    record.arg1 = arg1;
    result = nrf_log_protocol_encode_record(&record, serialized);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }

    key = lock_ring(ring);
    result = last_uptime_locked(ring, uptime_ms);
    if (result == NRF_LOG_ERROR_OK && ring->next_seq == NRF_LOG_RING_ROLLOVER_SEQ) {
        result = start_generation_locked(ring, uptime_ms);
    }
    if (result == NRF_LOG_ERROR_OK) {
        record.seq = ring->next_seq;
        result = write_record_locked(ring, &record);
    }
    unlock_ring(ring, key);
    return result;
}

nrf_log_error_t nrf_log_ring_status_snapshot(
    nrf_log_ring_t *ring,
    nrf_log_status_t *status)
{
    nrf_log_lock_key_t key;
    if (ring == NULL || status == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    key = lock_ring(ring);
    snapshot_locked(ring, 0U, status);
    unlock_ring(ring, key);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_ring_build_status_frame(
    nrf_log_ring_t *ring,
    uint32_t request_id,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length)
{
    nrf_log_cache_key_t cache_key;
    nrf_log_lock_key_t key;
    nrf_log_status_t status;
    uint8_t encoded[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    nrf_log_error_t result;

    if (ring == NULL || frame == NULL || frame_length == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (frame_capacity < NRF_LOG_PROTOCOL_STATUS_LENGTH) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    cache_key.request_type = NRF_LOG_MESSAGE_STATUS_REQUEST;
    cache_key.request_id = request_id;
    cache_key.expected_boot_id = 0U;
    cache_key.cursor_seq = 0U;
    cache_key.max_records = 0U;
    key = lock_ring(ring);
    if (cache_key_matches(ring, &cache_key)) {
        result = return_cached(ring, frame, frame_capacity, frame_length);
        unlock_ring(ring, key);
        return result;
    }
    snapshot_locked(ring, request_id, &status);
    result = nrf_log_protocol_encode_status(&status, encoded);
    if (result == NRF_LOG_ERROR_OK) {
        save_cache(ring, &cache_key, encoded, sizeof(encoded));
        memcpy(frame, encoded, sizeof(encoded));
        *frame_length = sizeof(encoded);
    }
    unlock_ring(ring, key);
    return result;
}

nrf_log_error_t nrf_log_ring_build_block_frame(
    nrf_log_ring_t *ring,
    uint32_t request_id,
    uint32_t expected_boot_id,
    uint32_t cursor_seq,
    uint8_t max_records,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length)
{
    nrf_log_cache_key_t cache_key;
    nrf_log_lock_key_t key;
    nrf_log_block_t block;
    nrf_log_block_validation_t validation;
    uint8_t encoded[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t encoded_length = 0U;
    uint32_t start_seq;
    uint32_t remaining;
    uint8_t oldest_slot;
    uint8_t i;
    nrf_log_error_t result;

    if (ring == NULL || frame == NULL || frame_length == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (max_records == 0U || max_records > NRF_LOG_PROTOCOL_MAX_RECORDS) {
        return NRF_LOG_ERROR_COUNT_OVERFLOW;
    }
    cache_key.request_type = NRF_LOG_MESSAGE_READ_REQUEST;
    cache_key.request_id = request_id;
    cache_key.expected_boot_id = expected_boot_id;
    cache_key.cursor_seq = cursor_seq;
    cache_key.max_records = max_records;
    key = lock_ring(ring);
    if (cache_key_matches(ring, &cache_key)) {
        result = return_cached(ring, frame, frame_capacity, frame_length);
        unlock_ring(ring, key);
        return result;
    }
    if (expected_boot_id == ring->boot_id && cursor_seq > ring->next_seq) {
        unlock_ring(ring, key);
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    memset(&block, 0, sizeof(block));
    block.request_id = request_id;
    block.boot_id = ring->boot_id;
    block.oldest_seq = ring->oldest_seq;
    block.overwritten_total = ring->overwritten_total;
    if (expected_boot_id != ring->boot_id) {
        block.flags = NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH;
        start_seq = ring->next_seq;
    } else if (cursor_seq < ring->oldest_seq) {
        block.flags = NRF_LOG_BLOCK_FLAG_GAP;
        start_seq = ring->oldest_seq;
    } else {
        start_seq = cursor_seq;
    }
    block.first_seq = start_seq;
    remaining = ring->next_seq - start_seq;
    block.record_count = (uint8_t)(remaining < max_records ? remaining : max_records);
    if (start_seq + block.record_count < ring->next_seq &&
        expected_boot_id == ring->boot_id) {
        block.flags |= NRF_LOG_BLOCK_FLAG_MORE;
    }
    block.next_cursor = start_seq + block.record_count;
    oldest_slot = oldest_index(ring);
    for (i = 0U; i < block.record_count; ++i) {
        uint32_t sequence_offset = start_seq + i - ring->oldest_seq;
        uint8_t slot = (uint8_t)((oldest_slot + sequence_offset) % NRF_LOG_RING_CAPACITY);
        result = nrf_log_protocol_decode_record(ring->records[slot], &block.records[i]);
        if (result != NRF_LOG_ERROR_OK) {
            unlock_ring(ring, key);
            return NRF_LOG_ERROR_INVARIANT_VIOLATION;
        }
    }
    memset(&validation, 0, sizeof(validation));
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = expected_boot_id;
    validation.has_cursor = true;
    validation.cursor_seq = cursor_seq;
    validation.has_max_records = true;
    validation.max_records = max_records;
    validation.has_status_next_seq = true;
    validation.status_next_seq = ring->next_seq;
    result = nrf_log_protocol_encode_block(&block, encoded, &encoded_length);
    if (result == NRF_LOG_ERROR_OK) {
        result = nrf_log_protocol_decode_block(encoded, encoded_length, &validation, &block);
    }
    if (result == NRF_LOG_ERROR_OK) {
        if (frame_capacity < encoded_length) {
            result = NRF_LOG_ERROR_BAD_LENGTH;
        } else {
            save_cache(ring, &cache_key, encoded, encoded_length);
            memcpy(frame, encoded, encoded_length);
            *frame_length = encoded_length;
        }
    }
    unlock_ring(ring, key);
    return result;
}

#ifdef NRF_LOG_RING_TEST_HOOKS
nrf_log_error_t nrf_log_ring_test_set_next_seq(nrf_log_ring_t *ring, uint32_t next_seq)
{
    nrf_log_lock_key_t key;
    if (ring == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    key = lock_ring(ring);
    if (ring->count != 0U) {
        unlock_ring(ring, key);
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    ring->oldest_seq = next_seq;
    ring->next_seq = next_seq;
    ring->cache_valid = 0U;
    unlock_ring(ring, key);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_ring_test_set_totals(
    nrf_log_ring_t *ring,
    uint32_t overwritten_total,
    uint32_t lost_internal_total)
{
    nrf_log_lock_key_t key;
    if (ring == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    key = lock_ring(ring);
    ring->overwritten_total = overwritten_total;
    ring->lost_internal_total = lost_internal_total;
    unlock_ring(ring, key);
    return NRF_LOG_ERROR_OK;
}
#endif
