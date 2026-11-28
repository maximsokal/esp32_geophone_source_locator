#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB2_PROTO_MAGIC_0           0x50U /* 'P' */
#define PB2_PROTO_MAGIC_1           0x42U /* 'B' */
#define PB2_PROTO_VERSION           1U
#define PB2_PROTO_CHANNEL_COUNT     4U

#define PB2_PROTO_HEADER_BYTES      20U
#define PB2_PROTO_CRC_BYTES         4U
#define PB2_PROTO_MAX_PAYLOAD       960U
#define PB2_PROTO_MAX_FRAME_BYTES   (PB2_PROTO_HEADER_BYTES + PB2_PROTO_MAX_PAYLOAD + PB2_PROTO_CRC_BYTES)
#define PB2_PROTO_MAX_COBS_BYTES    (PB2_PROTO_MAX_FRAME_BYTES + (PB2_PROTO_MAX_FRAME_BYTES / 254U) + 2U)
#define PB2_PROTO_PREVIEW_MAX_FRAMES 32U

#define PB2_PREVIEW_FLAG_GAP_BEFORE 0x0001U

typedef enum {
    PB2_MSG_HELLO               = 0x01,
    PB2_MSG_CAPABILITIES        = 0x02,
    PB2_MSG_HEARTBEAT           = 0x03,
    PB2_MSG_PING                = 0x04,
    PB2_MSG_PONG                = 0x05,
    PB2_MSG_ACK                 = 0x06,
    PB2_MSG_ERROR               = 0x07,
    PB2_MSG_ACQUISITION_STATUS  = 0x08,
    PB2_MSG_PREVIEW_BLOCK       = 0x09,

    /* Reserved in v1 contract for Phases 3-7 */
    PB2_MSG_CONFIGURE           = 0x10,
    PB2_MSG_START_ACQUISITION   = 0x11,
    PB2_MSG_STOP_ACQUISITION    = 0x12,
    PB2_MSG_SET_SAMPLE_RATE     = 0x13,
    PB2_MSG_SENSOR_HEALTH       = 0x14,
    PB2_MSG_DSP_RESULT          = 0x20,
    PB2_MSG_LOCALIZATION_RESULT = 0x21,
    PB2_MSG_GEOMETRY_UPDATE     = 0x22,
    PB2_MSG_STORAGE_STATUS      = 0x23,
    PB2_MSG_SHUTDOWN_REQUEST    = 0x30,
    PB2_MSG_SHUTDOWN_READY      = 0x31
} pb2_msg_type_t;

typedef enum {
    PB2_CLOCK_UNKNOWN           = 0,
    PB2_CLOCK_ESP32_US          = 1,
    PB2_CLOCK_PB2_PRU_TICKS     = 2,
    PB2_CLOCK_PB2_LINUX_MONO_US = 3,
    PB2_CLOCK_PB2_PRU_US        = 4
} pb2_clock_domain_t;

typedef enum {
    PB2_SAMPLE_FMT_FLOAT32_UV   = 1,
    PB2_SAMPLE_FMT_INT16_ADC    = 2
} pb2_sample_format_t;

typedef enum {
    PB2_PROTO_OK = 0,
    PB2_PROTO_ERR_INVALID_ARG,
    PB2_PROTO_ERR_BUFFER_TOO_SMALL,
    PB2_PROTO_ERR_BAD_MAGIC,
    PB2_PROTO_ERR_UNSUPPORTED_VERSION,
    PB2_PROTO_ERR_OVERSIZED,
    PB2_PROTO_ERR_TRUNCATED,
    PB2_PROTO_ERR_BAD_CRC,
    PB2_PROTO_ERR_COBS_DECODE,
    PB2_PROTO_ERR_UNSUPPORTED_MSG
} pb2_proto_status_t;

typedef struct {
    uint8_t version;
    uint8_t msg_type;
    uint8_t flags;
    uint32_t sequence;
    uint32_t boot_epoch;
    uint32_t session_epoch;
    uint16_t payload_len;
} pb2_frame_header_t;

typedef struct {
    uint8_t node_role; /* 1 = ESP32-S3, 2 = PocketBeagle2-A0 */
    uint8_t hw_rev;
    uint16_t proto_max_payload;
    uint32_t features_mask;
} pb2_hello_payload_t;

typedef struct {
    uint32_t supported_rates_mask;
    uint32_t max_baud_rate;
    uint8_t adc_owner; /* 1 = ESP32, 2 = PB2 */
    uint8_t channel_count;
} pb2_capabilities_payload_t;

typedef struct {
    uint64_t uptime_ms;
    uint32_t rx_frames;
    uint32_t dropped_frames;
} pb2_heartbeat_payload_t;

typedef struct {
    uint32_t nonce;
    uint64_t timestamp_us;
    uint8_t clock_domain;
} pb2_ping_pong_payload_t;

typedef struct {
    uint32_t acked_sequence;
    uint8_t acked_msg_type;
    uint8_t status_code;
} pb2_ack_payload_t;

typedef struct {
    uint32_t related_sequence;
    uint16_t error_code;
    uint8_t severity;
} pb2_error_payload_t;

typedef struct {
    uint8_t active;
    uint8_t source_id;
    uint8_t channel_count;
    uint8_t sample_format;
    uint32_t sample_rate_hz;
    uint64_t total_frames;
    uint64_t dropped_frames;
} pb2_acq_status_payload_t;

typedef struct {
    uint64_t first_sample_index;
    uint64_t first_timestamp_us;
    uint32_t source_sample_rate_hz;
    uint16_t decimation_factor;
    uint16_t frame_count;
    uint16_t block_flags;
    uint8_t clock_domain;
    uint8_t sample_format;
    uint8_t channel_count;
    uint8_t channel_map[PB2_PROTO_CHANNEL_COUNT];
    float samples_uv[PB2_PROTO_PREVIEW_MAX_FRAMES][PB2_PROTO_CHANNEL_COUNT];
} pb2_preview_block_t;

typedef struct {
    uint8_t rx_buf[PB2_PROTO_MAX_COBS_BYTES];
    size_t rx_len;
    bool overflowed;
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t cobs_errors;
    uint32_t header_errors;
    uint32_t oversized_errors;
} pb2_stream_decoder_t;

/* CRC32C (Castagnoli, poly 0x82F63B78) */
uint32_t pb2_crc32c(const void *data, size_t len);

/* COBS encode/decode (does not append/expect 0x00 delimiter) */
size_t pb2_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);
pb2_proto_status_t pb2_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

/* Raw frame encode/decode (header + payload + CRC32C, little-endian) */
pb2_proto_status_t pb2_frame_encode(
    const pb2_frame_header_t *hdr,
    const uint8_t *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len
);

pb2_proto_status_t pb2_frame_decode(
    const uint8_t *frame,
    size_t frame_len,
    pb2_frame_header_t *out_hdr,
    const uint8_t **out_payload
);

/* Encode COBS + trailing 0x00 delimiter directly into wire buffer */
pb2_proto_status_t pb2_wire_encode(
    const pb2_frame_header_t *hdr,
    const uint8_t *payload,
    uint8_t *out_wire,
    size_t out_cap,
    size_t *out_len
);

/* Payload codecs */
pb2_proto_status_t pb2_encode_hello(const pb2_hello_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_hello(const uint8_t *buf, size_t len, pb2_hello_payload_t *p);

pb2_proto_status_t pb2_encode_capabilities(const pb2_capabilities_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_capabilities(const uint8_t *buf, size_t len, pb2_capabilities_payload_t *p);

pb2_proto_status_t pb2_encode_heartbeat(const pb2_heartbeat_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_heartbeat(const uint8_t *buf, size_t len, pb2_heartbeat_payload_t *p);

pb2_proto_status_t pb2_encode_ping_pong(const pb2_ping_pong_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_ping_pong(const uint8_t *buf, size_t len, pb2_ping_pong_payload_t *p);

pb2_proto_status_t pb2_encode_ack(const pb2_ack_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_ack(const uint8_t *buf, size_t len, pb2_ack_payload_t *p);

pb2_proto_status_t pb2_encode_error(const pb2_error_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_error(const uint8_t *buf, size_t len, pb2_error_payload_t *p);

pb2_proto_status_t pb2_encode_acq_status(const pb2_acq_status_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_acq_status(const uint8_t *buf, size_t len, pb2_acq_status_payload_t *p);

pb2_proto_status_t pb2_encode_preview_block(const pb2_preview_block_t *b, uint8_t *buf, size_t cap, uint16_t *len);
pb2_proto_status_t pb2_decode_preview_block(const uint8_t *buf, size_t len, pb2_preview_block_t *b);

/* Streaming byte-by-byte / chunked COBS+CRC32C decoder */
void pb2_stream_decoder_init(pb2_stream_decoder_t *dec);
typedef void (*pb2_stream_frame_cb_t)(
    const pb2_frame_header_t *hdr,
    const uint8_t *payload,
    void *user_ctx
);
void pb2_stream_decoder_feed(
    pb2_stream_decoder_t *dec,
    const uint8_t *bytes,
    size_t len,
    pb2_stream_frame_cb_t on_frame,
    void *user_ctx
);

bool pb2_msg_type_is_phase2_supported(uint8_t msg_type);

#ifdef __cplusplus
}
#endif
