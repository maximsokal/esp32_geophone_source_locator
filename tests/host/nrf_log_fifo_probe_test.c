#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nrf_log_collector.h"
#include "nrf_log_protocol.h"

typedef struct {
    uint8_t prepared[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    size_t lengths[12];
    unsigned calls;
    unsigned fail_call;
    unsigned short_call;
    unsigned change_call;
} fake_reply_t;

static void prepare(fake_reply_t *reply)
{
    const nrf_log_status_t status = {
        .request_id = 7U, .flags = NRF_LOG_STATUS_FLAG_ENABLED, .capacity = 128U,
        .boot_id = 2U, .oldest_seq = 1U, .next_seq = 1U,
    };
    memset(reply, 0, sizeof(*reply));
    assert(nrf_log_protocol_encode_status(&status, reply->prepared) == NRF_LOG_ERROR_OK);
}

static esp_err_t read_reply(void *context, uint8_t node, uint8_t *frame,
                            size_t capacity, size_t *received_length)
{
    fake_reply_t *reply = context;
    assert(node == 2U && capacity <= sizeof(reply->prepared));
    assert(reply->calls < sizeof(reply->lengths) / sizeof(reply->lengths[0]));
    reply->lengths[reply->calls++] = capacity;
    *received_length = capacity;
    if (reply->calls == reply->fail_call) return ESP_ERR_TIMEOUT;
    memcpy(frame, reply->prepared, capacity);
    if (reply->calls == reply->short_call) --*received_length;
    if (reply->calls == reply->change_call) frame[6] ^= 1U;
    return ESP_OK;
}

int main(void)
{
    fake_reply_t reply;
    uint8_t next = 0U;
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    size_t received;
    const nrf_log_collector_io_t io = {.context = &reply, .read_frame = read_reply};
    const size_t expected[] = {28U, 38U, 31U, 38U, 32U, 38U, 33U, 38U, 38U};
    prepare(&reply);
    for (unsigned i = 0U; i < 5U; ++i) {
        assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, &received) == ESP_OK);
        assert(received == sizeof(frame));
        assert(memcmp(frame, reply.prepared, sizeof(frame)) == 0);
        nrf_log_status_t decoded;
        assert(nrf_log_protocol_decode_status(frame, received, &decoded) == NRF_LOG_ERROR_OK);
        assert(decoded.request_id == 7U);
    }
    assert(next == 0U && reply.calls == sizeof(expected) / sizeof(expected[0]));
    assert(memcmp(reply.lengths, expected, sizeof(expected)) == 0);

    prepare(&reply);
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, false, frame, &received) == ESP_OK);
    assert(reply.calls == 1U && reply.lengths[0] == 38U && next == 0U);
    for (uint8_t step = 0U; step < 5U; ++step) {
        prepare(&reply);
        next = step;
        reply.fail_call = 1U;
        received = 999U;
        assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, &received) == ESP_ERR_TIMEOUT);
        assert(received == 0U && reply.calls == 1U);
        assert(next == (uint8_t)((step + 1U) % 5U));
    }
    prepare(&reply);
    next = 0U;
    reply.fail_call = 2U;
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, &received) == ESP_ERR_TIMEOUT);
    assert(received == 0U && reply.calls == 2U);

    for (unsigned short_call = 1U; short_call <= 2U; ++short_call) {
        prepare(&reply);
        next = 0U;
        reply.short_call = short_call;
        assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, &received) == ESP_ERR_INVALID_RESPONSE);
        assert(received == 0U && reply.calls == short_call);
    }
    prepare(&reply);
    next = 3U;
    reply.change_call = 2U;
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, &received) == ESP_ERR_INVALID_RESPONSE);
    assert(received == 0U);

    prepare(&reply);
    assert(nrf_log_collector_read_status_with_probe(NULL, 2U, &next, true, frame, &received) == ESP_ERR_INVALID_ARG);
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, NULL, true, frame, &received) == ESP_ERR_INVALID_ARG);
    assert(nrf_log_collector_read_status_with_probe(&io, NRF_LOG_COLLECTOR_NODE_COUNT, &next, true, frame, &received) == ESP_ERR_INVALID_ARG);
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, NULL, &received) == ESP_ERR_INVALID_ARG);
    assert(nrf_log_collector_read_status_with_probe(&io, 2U, &next, true, frame, NULL) == ESP_ERR_INVALID_ARG);
    assert(reply.calls == 0U);
    puts("nRF prepared-status FIFO probes: PASS");
    return 0;
}
