#include "pb2_protocol.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(cond)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "Check failed: %s (%s:%d)\n", #cond,            \
                    __FILE__, __LINE__);                                    \
            abort();                                                        \
        }                                                                   \
    } while (0)

typedef struct {
    size_t count;
    pb2_frame_header_t last_hdr;
    uint8_t last_payload[PB2_PROTO_MAX_PAYLOAD];
} capture_ctx_t;

static void on_captured_frame(const pb2_frame_header_t *hdr, const uint8_t *payload, void *ctx) {
    capture_ctx_t *c = (capture_ctx_t *)ctx;
    c->last_hdr = *hdr;
    if (hdr->payload_len > 0U && payload != NULL) {
        memcpy(c->last_payload, payload, hdr->payload_len);
    }
    ++c->count;
}

static void test_crc32c_standard_vector(void) {
    const char *vec = "123456789";
    TEST_CHECK(pb2_crc32c(vec, 9U) == 0xE3069283U);
}

static void test_cobs_roundtrip_and_errors(void) {
    const uint8_t data[] = {0x00, 0x11, 0x00, 0x22, 0x33, 0x00};
    uint8_t enc[32];
    uint8_t dec[32];
    size_t enc_len = pb2_cobs_encode(data, sizeof(data), enc, sizeof(enc));
    TEST_CHECK(enc_len == 7U);
    for (size_t i = 0U; i < enc_len; ++i) {
        TEST_CHECK(enc[i] != 0x00U);
    }
    size_t dec_len = 0U;
    TEST_CHECK(pb2_cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len) == PB2_PROTO_OK);
    TEST_CHECK(dec_len == sizeof(data));
    TEST_CHECK(memcmp(dec, data, sizeof(data)) == 0);

    enc[2] = 0x00U;
    TEST_CHECK(pb2_cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len) == PB2_PROTO_ERR_COBS_DECODE);
}

static void test_golden_hello_wire_vector(void) {
    const pb2_hello_payload_t hello = {
        .node_role = 1U,
        .hw_rev = 0x11U,
        .proto_max_payload = PB2_PROTO_MAX_PAYLOAD,
        .features_mask = 0x00000003U
    };
    uint8_t payload[16];
    uint16_t payload_len = 0U;
    TEST_CHECK(pb2_encode_hello(&hello, payload, sizeof(payload), &payload_len) == PB2_PROTO_OK);
    TEST_CHECK(payload_len == 8U);

    const pb2_frame_header_t hdr = {
        .version = PB2_PROTO_VERSION,
        .msg_type = PB2_MSG_HELLO,
        .flags = 0x00U,
        .sequence = 0x01020304U,
        .boot_epoch = 0x00000007U,
        .session_epoch = 0x00000009U,
        .payload_len = payload_len
    };

    uint8_t wire[PB2_PROTO_MAX_COBS_BYTES];
    size_t wire_len = 0U;
    TEST_CHECK(pb2_wire_encode(&hdr, payload, wire, sizeof(wire), &wire_len) == PB2_PROTO_OK);
    TEST_CHECK(wire_len > 24U);
    TEST_CHECK(wire[wire_len - 1U] == 0x00U);

    pb2_stream_decoder_t dec;
    pb2_stream_decoder_init(&dec);
    capture_ctx_t cap = {0};
    /* Feed 1 byte at a time (fragmented) */
    for (size_t i = 0U; i < wire_len; ++i) {
        pb2_stream_decoder_feed(&dec, &wire[i], 1U, on_captured_frame, &cap);
    }
    TEST_CHECK(cap.count == 1U);
    TEST_CHECK(dec.frames_ok == 1U);
    TEST_CHECK(cap.last_hdr.sequence == 0x01020304U);
    TEST_CHECK(cap.last_hdr.boot_epoch == 7U);
    TEST_CHECK(cap.last_hdr.session_epoch == 9U);

    pb2_hello_payload_t decoded = {0};
    TEST_CHECK(pb2_decode_hello(cap.last_payload, cap.last_hdr.payload_len, &decoded) == PB2_PROTO_OK);
    TEST_CHECK(decoded.node_role == 1U);
    TEST_CHECK(decoded.hw_rev == 0x11U);
    TEST_CHECK(decoded.proto_max_payload == PB2_PROTO_MAX_PAYLOAD);
    TEST_CHECK(decoded.features_mask == 0x00000003U);
}

static void test_preview_block_and_errors(void) {
    pb2_preview_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.first_sample_index = 100000ULL;
    blk.first_timestamp_us = 5000000ULL;
    blk.source_sample_rate_hz = 500U;
    blk.decimation_factor = 2U;
    blk.frame_count = 2U;
    blk.block_flags = PB2_PREVIEW_FLAG_GAP_BEFORE;
    blk.clock_domain = PB2_CLOCK_ESP32_US;
    blk.sample_format = PB2_SAMPLE_FMT_FLOAT32_UV;
    blk.channel_count = PB2_PROTO_CHANNEL_COUNT;
    blk.channel_map[0] = 1U;
    blk.channel_map[1] = 2U;
    blk.channel_map[2] = 3U;
    blk.channel_map[3] = 4U;
    blk.samples_uv[0][0] = 12.5f;
    blk.samples_uv[0][1] = -34.25f;
    blk.samples_uv[1][3] = 99.75f;

    uint8_t payload[PB2_PROTO_MAX_PAYLOAD];
    uint16_t plen = 0U;
    TEST_CHECK(pb2_encode_preview_block(&blk, payload, sizeof(payload), &plen) == PB2_PROTO_OK);
    TEST_CHECK(plen == 33U + 2U * 16U);

    pb2_preview_block_t out_blk;
    TEST_CHECK(pb2_decode_preview_block(payload, plen, &out_blk) == PB2_PROTO_OK);
    TEST_CHECK(out_blk.first_sample_index == 100000ULL);
    TEST_CHECK(out_blk.block_flags == PB2_PREVIEW_FLAG_GAP_BEFORE);
    TEST_CHECK(fabsf(out_blk.samples_uv[0][0] - 12.5f) < 1e-6f);
    TEST_CHECK(fabsf(out_blk.samples_uv[0][1] - (-34.25f)) < 1e-6f);
    TEST_CHECK(fabsf(out_blk.samples_uv[1][3] - 99.75f) < 1e-6f);

    /* Unknown version & oversized rejection */
    pb2_frame_header_t bad_ver = {
        .version = 99U, .msg_type = PB2_MSG_PING, .flags = 0U,
        .sequence = 1U, .boot_epoch = 1U, .session_epoch = 1U, .payload_len = 0U
    };
    uint8_t frame[64];
    size_t flen = 0U;
    TEST_CHECK(pb2_frame_encode(&bad_ver, NULL, frame, sizeof(frame), &flen) == PB2_PROTO_ERR_UNSUPPORTED_VERSION);

    pb2_frame_header_t oversized = bad_ver;
    oversized.version = PB2_PROTO_VERSION;
    oversized.payload_len = PB2_PROTO_MAX_PAYLOAD + 1U;
    TEST_CHECK(pb2_frame_encode(&oversized, payload, frame, sizeof(frame), &flen) == PB2_PROTO_ERR_OVERSIZED);

    /* Reserved Phase 3+ message types are not Phase 2 supported */
    TEST_CHECK(pb2_msg_type_is_phase2_supported(PB2_MSG_PREVIEW_BLOCK));
    TEST_CHECK(!pb2_msg_type_is_phase2_supported(PB2_MSG_DSP_RESULT));
}

int main(void) {
    test_crc32c_standard_vector();
    test_cobs_roundtrip_and_errors();
    test_golden_hello_wire_vector();
    test_preview_block_and_errors();
    printf("pb2_protocol_test: PASS\n");
    return 0;
}
