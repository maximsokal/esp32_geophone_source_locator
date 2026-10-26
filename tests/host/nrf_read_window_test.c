#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_read_window.h"
#include "nrf_upstream_protocol.h"

#define WINDOW NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES

static void fill_pattern(uint8_t *data, size_t length, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        data[index] = (uint8_t)(seed + (index * 7U) + (index >> 3U));
    }
}

/* ---- target ------------------------------------------------------------- */

static void test_target_serves_the_recorded_response_from_the_armed_offset(void)
{
    nrf_read_window_target_t target;
    uint8_t response[136];
    const uint8_t *data = NULL;
    uint32_t length = 0U;

    fill_pattern(response, sizeof(response), 3U);
    nrf_read_window_target_init(&target);
    nrf_read_window_target_record(&target, response, sizeof(response));

    assert(nrf_read_window_target_arm(&target, 28U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 136U - 28U);
    assert(memcmp(data, &response[28], length) == 0);

    /* one window per request: a read that was not preceded by a request is ordinary */
    assert(!nrf_read_window_target_serve(&target, &data, &length));

    /* the response is kept, so the next window of the same response follows */
    assert(nrf_read_window_target_arm(&target, 112U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 24U);
    assert(memcmp(data, &response[112], 24U) == 0);

    assert(nrf_read_window_target_arm(&target, 0U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 136U && memcmp(data, response, 136U) == 0);
}

static void test_target_serves_a_frozen_copy(void)
{
    nrf_read_window_target_t target;
    uint8_t response[56];
    uint8_t original[56];
    const uint8_t *data = NULL;
    uint32_t length = 0U;

    fill_pattern(response, sizeof(response), 9U);
    memcpy(original, response, sizeof(response));
    nrf_read_window_target_init(&target);
    nrf_read_window_target_record(&target, response, sizeof(response));
    memset(response, 0xEE, sizeof(response)); /* the live buffer is rebuilt afterwards */

    assert(nrf_read_window_target_arm(&target, 28U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 28U && memcmp(data, &original[28], 28U) == 0);
}

static void test_target_rejects_offsets_outside_the_response(void)
{
    nrf_read_window_target_t target;
    uint8_t response[38];
    const uint8_t *data = NULL;
    uint32_t length = 0U;

    fill_pattern(response, sizeof(response), 1U);
    nrf_read_window_target_init(&target);

    /* nothing recorded yet */
    assert(!nrf_read_window_target_arm(&target, 0U));
    assert(!nrf_read_window_target_serve(&target, &data, &length));

    nrf_read_window_target_record(&target, response, sizeof(response));
    assert(nrf_read_window_target_arm(&target, 37U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 1U && data[0] == response[37]);

    assert(!nrf_read_window_target_arm(&target, 38U));
    assert(!nrf_read_window_target_serve(&target, &data, &length));
    assert(!nrf_read_window_target_arm(&target, 136U));
    assert(!nrf_read_window_target_arm(&target, UINT16_MAX));
    assert(!nrf_read_window_target_serve(&target, &data, &length));

    /* a refused arm also cancels one that was pending */
    assert(nrf_read_window_target_arm(&target, 10U));
    assert(!nrf_read_window_target_arm(&target, 200U));
    assert(!nrf_read_window_target_serve(&target, &data, &length));
}

static void test_target_forgets_unusable_responses(void)
{
    nrf_read_window_target_t target;
    uint8_t response[NRF_READ_WINDOW_CAPACITY + 1U];
    const uint8_t *data = NULL;
    uint32_t length = 0U;

    fill_pattern(response, sizeof(response), 5U);
    nrf_read_window_target_init(&target);

    nrf_read_window_target_record(&target, response, NRF_READ_WINDOW_CAPACITY);
    assert(nrf_read_window_target_arm(&target, 135U));
    assert(nrf_read_window_target_serve(&target, &data, &length));
    assert(length == 1U && data[0] == response[135]);

    nrf_read_window_target_record(&target, response, NRF_READ_WINDOW_CAPACITY + 1U);
    assert(!nrf_read_window_target_arm(&target, 0U));

    nrf_read_window_target_record(&target, response, 38U);
    assert(nrf_read_window_target_arm(&target, 0U));
    nrf_read_window_target_record(&target, NULL, 38U);
    assert(!nrf_read_window_target_serve(&target, &data, &length));
    assert(!nrf_read_window_target_arm(&target, 0U));

    nrf_read_window_target_record(&target, response, 38U);
    nrf_read_window_target_record(&target, response, 0U);
    assert(!nrf_read_window_target_arm(&target, 0U));
}

static void test_target_record_and_disarm_cancel_a_pending_window(void)
{
    nrf_read_window_target_t target;
    uint8_t response[56];
    const uint8_t *data = NULL;
    uint32_t length = 0U;

    fill_pattern(response, sizeof(response), 2U);
    nrf_read_window_target_init(&target);
    nrf_read_window_target_record(&target, response, sizeof(response));

    assert(nrf_read_window_target_arm(&target, 28U));
    nrf_read_window_target_disarm(&target);
    assert(!nrf_read_window_target_serve(&target, &data, &length));

    /* a fresh response replaces the old one; a stale window must not survive it */
    assert(nrf_read_window_target_arm(&target, 28U));
    nrf_read_window_target_record(&target, response, 28U);
    assert(!nrf_read_window_target_serve(&target, &data, &length));
    assert(!nrf_read_window_target_arm(&target, 28U));
}

static void test_target_null_arguments_are_harmless(void)
{
    nrf_read_window_target_t target;
    const uint8_t *data = NULL;
    uint32_t length = 0U;
    uint8_t response[28] = {0};

    nrf_read_window_target_init(NULL);
    nrf_read_window_target_record(NULL, response, sizeof(response));
    nrf_read_window_target_disarm(NULL);
    assert(!nrf_read_window_target_arm(NULL, 0U));
    assert(!nrf_read_window_target_serve(NULL, &data, &length));

    nrf_read_window_target_init(&target);
    nrf_read_window_target_record(&target, response, sizeof(response));
    assert(nrf_read_window_target_arm(&target, 0U));
    assert(!nrf_read_window_target_serve(&target, NULL, &length));
    assert(!nrf_read_window_target_serve(&target, &data, NULL));
}


/* ---- client against a modelled target ----------------------------------- */

typedef struct {
    nrf_read_window_target_t target;
    uint8_t response[NRF_READ_WINDOW_CAPACITY];
    size_t response_length;
    size_t calls;
    size_t reads;
    size_t writes;
    size_t longest_read;
    size_t fail_at_call; /* 1-based; 0 = never */
    int fail_code;
    uint16_t offsets[8];
    int bad_window_frames;
} link_t;

static void link_init(link_t *link, size_t response_length, uint8_t seed)
{
    memset(link, 0, sizeof(*link));
    nrf_read_window_target_init(&link->target);
    link->response_length = response_length;
    fill_pattern(link->response, response_length, seed);
}

static int link_write_window(
    void *context,
    const uint8_t frame[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH])
{
    link_t *link = context;
    nrf_upstream_read_window_t window;

    link->calls++;
    if (link->fail_at_call != 0U && link->calls == link->fail_at_call) {
        return link->fail_code;
    }
    if (nrf_upstream_protocol_decode_read_window(
            frame, NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH, &window) != NRF_UPSTREAM_RESULT_OK) {
        link->bad_window_frames++;
        nrf_read_window_target_disarm(&link->target);
        return 0;
    }
    if (link->writes < sizeof(link->offsets) / sizeof(link->offsets[0])) {
        link->offsets[link->writes] = window.offset;
    }
    link->writes++;
    (void)nrf_read_window_target_arm(&link->target, window.offset);
    return 0;
}

static int link_read(void *context, uint8_t *data, size_t length)
{
    link_t *link = context;
    const uint8_t *served = NULL;
    uint32_t served_length = 0U;
    size_t index;

    link->calls++;
    if (link->fail_at_call != 0U && link->calls == link->fail_at_call) {
        return link->fail_code;
    }
    link->reads++;
    if (length > link->longest_read) {
        link->longest_read = length;
    }
    if (nrf_read_window_target_serve(&link->target, &served, &served_length)) {
        for (index = 0U; index < length; ++index) {
            /* an over-read is answered with a character that never occurs in the response */
            data[index] = index < served_length ? served[index] : 0xEEU;
        }
        return 0;
    }
    /* an ordinary read: the nRF builds the response afresh, hands it out and remembers it */
    nrf_read_window_target_record(&link->target, link->response, link->response_length);
    for (index = 0U; index < length; ++index) {
        data[index] = index < link->response_length ? link->response[index] : 0xEEU;
    }
    return 0;
}

static nrf_read_window_io_t link_io(link_t *link)
{
    nrf_read_window_io_t io;
    io.context = link;
    io.write_window = link_write_window;
    io.read = link_read;
    return io;
}

static void test_client_assembles_every_length_exactly(void)
{
    size_t length;

    for (length = 1U; length <= NRF_READ_WINDOW_CAPACITY; ++length) {
        link_t link;
        nrf_read_window_io_t io;
        uint8_t frame[NRF_READ_WINDOW_CAPACITY + 4U];
        const size_t reads = (length + WINDOW - 1U) / WINDOW;

        link_init(&link, length, (uint8_t)length);
        io = link_io(&link);
        memset(frame, 0xCC, sizeof(frame));

        assert(nrf_read_window_client_read(&io, frame, length) == 0);
        assert(memcmp(frame, link.response, length) == 0);
        assert(frame[length] == 0xCC && frame[length + 3U] == 0xCC); /* no write past the end */
        assert(link.reads == reads);
        assert(link.writes == reads - 1U);
        assert(link.bad_window_frames == 0);
        assert(link.longest_read <= WINDOW); /* never a segmented transfer */
    }
}

static void test_client_short_responses_are_one_plain_read(void)
{
    size_t length;

    for (length = 1U; length <= WINDOW; ++length) {
        link_t link;
        nrf_read_window_io_t io;
        uint8_t frame[NRF_READ_WINDOW_CAPACITY];

        link_init(&link, length, 4U);
        io = link_io(&link);
        assert(nrf_read_window_client_read(&io, frame, length) == 0);
        assert(link.calls == 1U && link.reads == 1U && link.writes == 0U);
        assert(link.longest_read == length);
    }
}

static void test_client_requests_consecutive_window_offsets(void)
{
    link_t link;
    nrf_read_window_io_t io;
    uint8_t frame[NRF_READ_WINDOW_CAPACITY];

    link_init(&link, 136U, 11U);
    io = link_io(&link);
    assert(nrf_read_window_client_read(&io, frame, 136U) == 0);
    assert(link.writes == 4U && link.reads == 5U);
    assert(link.offsets[0] == 28U && link.offsets[1] == 56U &&
           link.offsets[2] == 84U && link.offsets[3] == 112U);
    assert(memcmp(frame, link.response, 136U) == 0);

    /* a log block with two records (76 bytes) and the 38 byte status */
    link_init(&link, 76U, 12U);
    io = link_io(&link);
    assert(nrf_read_window_client_read(&io, frame, 76U) == 0);
    assert(link.writes == 2U && link.offsets[0] == 28U && link.offsets[1] == 56U);
    link_init(&link, 38U, 13U);
    io = link_io(&link);
    assert(nrf_read_window_client_read(&io, frame, 38U) == 0);
    assert(link.writes == 1U && link.offsets[0] == 28U);
    assert(memcmp(frame, link.response, 38U) == 0);
}

static void test_client_reports_the_first_failure_and_stops(void)
{
    const int codes[] = {-1, 0x103, 0x7F01, 0x7F02, 0x107};
    size_t code_index;

    for (code_index = 0U; code_index < sizeof(codes) / sizeof(codes[0]); ++code_index) {
        size_t fail_at;
        for (fail_at = 1U; fail_at <= 9U; ++fail_at) { /* 5 reads + 4 window writes */
            link_t link;
            nrf_read_window_io_t io;
            uint8_t frame[NRF_READ_WINDOW_CAPACITY];

            link_init(&link, 136U, 21U);
            io = link_io(&link);
            link.fail_at_call = fail_at;
            link.fail_code = codes[code_index];
            assert(nrf_read_window_client_read(&io, frame, 136U) == codes[code_index]);
            assert(link.calls == fail_at); /* nothing is attempted after the failure */
        }
    }
}

static void test_client_rejects_invalid_arguments_without_touching_the_bus(void)
{
    link_t link;
    nrf_read_window_io_t io;
    nrf_read_window_io_t no_read;
    nrf_read_window_io_t no_write;
    uint8_t frame[NRF_READ_WINDOW_CAPACITY + 1U];

    link_init(&link, 136U, 1U);
    io = link_io(&link);
    no_read = io;
    no_read.read = NULL;
    no_write = io;
    no_write.write_window = NULL;

    assert(nrf_read_window_client_read(NULL, frame, 28U) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&no_read, frame, 28U) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&no_write, frame, 28U) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&io, NULL, 28U) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&io, frame, 0U) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&io, frame, NRF_READ_WINDOW_CAPACITY + 1U) ==
           NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(nrf_read_window_client_read(&io, frame, SIZE_MAX) == NRF_READ_WINDOW_ERR_ARGUMENT);
    assert(link.calls == 0U);
}

static void test_windows_come_from_one_frozen_response(void)
{
    /*
     * The nRF rebuilds its live response on every ordinary read (a fresh
     * snapshot, a new log block). The windows must still belong to the response
     * the first read selected.
     */
    link_t link;
    uint8_t request[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
    const nrf_upstream_read_window_t window = {.offset = 28U};
    uint8_t first[28];
    uint8_t second[28];
    uint8_t original[NRF_READ_WINDOW_CAPACITY];

    link_init(&link, 136U, 31U);
    memcpy(original, link.response, 136U);
    assert(link_read(&link, first, sizeof(first)) == 0);
    assert(memcmp(first, original, sizeof(first)) == 0);
    fill_pattern(link.response, 136U, 99U); /* live response replaced */

    assert(nrf_upstream_protocol_encode_read_window(&window, request) == NRF_UPSTREAM_RESULT_OK);
    assert(link_write_window(&link, request) == 0);
    assert(link_read(&link, second, sizeof(second)) == 0);
    assert(memcmp(second, &original[28], sizeof(second)) == 0);
}

static void test_a_malformed_window_request_arms_nothing(void)
{
    link_t link;
    uint8_t request[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
    const nrf_upstream_read_window_t window = {.offset = 28U};
    uint8_t first[28];
    uint8_t second[28];

    link_init(&link, 136U, 41U);
    assert(link_read(&link, first, sizeof(first)) == 0);
    assert(nrf_upstream_protocol_encode_read_window(&window, request) == NRF_UPSTREAM_RESULT_OK);
    request[6] ^= 0x04U; /* corrupted in transit, CRC no longer matches */
    assert(link_write_window(&link, request) == 0);
    assert(link.bad_window_frames == 1 && link.writes == 0U);

    /* the next read is an ordinary one: it starts again at the head of the response */
    assert(link_read(&link, second, sizeof(second)) == 0);
    assert(memcmp(second, link.response, sizeof(second)) == 0);
}

int main(void)
{
    test_target_serves_the_recorded_response_from_the_armed_offset();
    test_target_serves_a_frozen_copy();
    test_target_rejects_offsets_outside_the_response();
    test_target_forgets_unusable_responses();
    test_target_record_and_disarm_cancel_a_pending_window();
    test_target_null_arguments_are_harmless();
    test_client_assembles_every_length_exactly();
    test_client_short_responses_are_one_plain_read();
    test_client_requests_consecutive_window_offsets();
    test_client_reports_the_first_failure_and_stops();
    test_client_rejects_invalid_arguments_without_touching_the_bus();
    test_windows_come_from_one_frozen_response();
    test_a_malformed_window_request_arms_nothing();
    return 0;
}
