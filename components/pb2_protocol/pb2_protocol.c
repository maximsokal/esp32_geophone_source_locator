#include "pb2_protocol.h"

#include <string.h>

static void write_u16_le(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void write_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((v >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static void write_u64_le(uint8_t *dst, uint64_t v) {
    write_u32_le(dst, (uint32_t)(v & 0xFFFFFFFFULL));
    write_u32_le(dst + 4U, (uint32_t)((v >> 32U) & 0xFFFFFFFFULL));
}

static void write_f32_le(uint8_t *dst, float f) {
    uint32_t u = 0U;
    memcpy(&u, &f, sizeof(u));
    write_u32_le(dst, u);
}

static uint16_t read_u16_le(const uint8_t *src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8U));
}

static uint32_t read_u32_le(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8U) |
           ((uint32_t)src[2] << 16U) |
           ((uint32_t)src[3] << 24U);
}

static uint64_t read_u64_le(const uint8_t *src) {
    return ((uint64_t)read_u32_le(src)) |
           (((uint64_t)read_u32_le(src + 4U)) << 32U);
}

static float read_f32_le(const uint8_t *src) {
    const uint32_t u = read_u32_le(src);
    float f = 0.0f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

bool pb2_msg_type_is_phase2_supported(uint8_t msg_type) {
    switch (msg_type) {
        case PB2_MSG_HELLO:
        case PB2_MSG_CAPABILITIES:
        case PB2_MSG_HEARTBEAT:
        case PB2_MSG_PING:
        case PB2_MSG_PONG:
        case PB2_MSG_ACK:
        case PB2_MSG_ERROR:
        case PB2_MSG_ACQUISITION_STATUS:
        case PB2_MSG_PREVIEW_BLOCK:
            return true;
        default:
            return false;
    }
}

pb2_proto_status_t pb2_frame_encode(
    const pb2_frame_header_t *hdr,
    const uint8_t *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len
) {
    if (hdr == NULL || out_frame == NULL || out_len == NULL) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }
    if (hdr->version != PB2_PROTO_VERSION) {
        return PB2_PROTO_ERR_UNSUPPORTED_VERSION;
    }
    if (hdr->payload_len > PB2_PROTO_MAX_PAYLOAD) {
        return PB2_PROTO_ERR_OVERSIZED;
    }
    if (hdr->payload_len > 0U && payload == NULL) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }

    const size_t total = (size_t)PB2_PROTO_HEADER_BYTES + (size_t)hdr->payload_len + (size_t)PB2_PROTO_CRC_BYTES;
    if (out_cap < total) {
        return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    }

    out_frame[0] = PB2_PROTO_MAGIC_0;
    out_frame[1] = PB2_PROTO_MAGIC_1;
    out_frame[2] = hdr->version;
    out_frame[3] = hdr->msg_type;
    out_frame[4] = hdr->flags;
    out_frame[5] = 0U;
    write_u16_le(out_frame + 6U, hdr->payload_len);
    write_u32_le(out_frame + 8U, hdr->sequence);
    write_u32_le(out_frame + 12U, hdr->boot_epoch);
    write_u32_le(out_frame + 16U, hdr->session_epoch);

    if (hdr->payload_len > 0U) {
        memcpy(out_frame + PB2_PROTO_HEADER_BYTES, payload, hdr->payload_len);
    }

    const uint32_t crc = pb2_crc32c(out_frame, PB2_PROTO_HEADER_BYTES + hdr->payload_len);
    write_u32_le(out_frame + PB2_PROTO_HEADER_BYTES + hdr->payload_len, crc);
    *out_len = total;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_frame_decode(
    const uint8_t *frame,
    size_t frame_len,
    pb2_frame_header_t *out_hdr,
    const uint8_t **out_payload
) {
    if (frame == NULL || out_hdr == NULL || out_payload == NULL) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }
    if (frame_len < PB2_PROTO_HEADER_BYTES + PB2_PROTO_CRC_BYTES) {
        return PB2_PROTO_ERR_TRUNCATED;
    }
    if (frame_len > PB2_PROTO_MAX_FRAME_BYTES) {
        return PB2_PROTO_ERR_OVERSIZED;
    }
    if (frame[0] != PB2_PROTO_MAGIC_0 || frame[1] != PB2_PROTO_MAGIC_1) {
        return PB2_PROTO_ERR_BAD_MAGIC;
    }
    if (frame[2] != PB2_PROTO_VERSION) {
        return PB2_PROTO_ERR_UNSUPPORTED_VERSION;
    }

    const uint16_t payload_len = read_u16_le(frame + 6U);
    if (payload_len > PB2_PROTO_MAX_PAYLOAD) {
        return PB2_PROTO_ERR_OVERSIZED;
    }
    const size_t expected_len = (size_t)PB2_PROTO_HEADER_BYTES + (size_t)payload_len + (size_t)PB2_PROTO_CRC_BYTES;
    if (frame_len != expected_len) {
        return PB2_PROTO_ERR_TRUNCATED;
    }

    const uint32_t expected_crc = read_u32_le(frame + PB2_PROTO_HEADER_BYTES + payload_len);
    const uint32_t actual_crc = pb2_crc32c(frame, PB2_PROTO_HEADER_BYTES + payload_len);
    if (expected_crc != actual_crc) {
        return PB2_PROTO_ERR_BAD_CRC;
    }

    out_hdr->version = frame[2];
    out_hdr->msg_type = frame[3];
    out_hdr->flags = frame[4];
    out_hdr->payload_len = payload_len;
    out_hdr->sequence = read_u32_le(frame + 8U);
    out_hdr->boot_epoch = read_u32_le(frame + 12U);
    out_hdr->session_epoch = read_u32_le(frame + 16U);
    *out_payload = (payload_len > 0U) ? (frame + PB2_PROTO_HEADER_BYTES) : NULL;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_wire_encode(
    const pb2_frame_header_t *hdr,
    const uint8_t *payload,
    uint8_t *out_wire,
    size_t out_cap,
    size_t *out_len
) {
    if (out_wire == NULL || out_len == NULL) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }
    uint8_t raw[PB2_PROTO_MAX_FRAME_BYTES];
    size_t raw_len = 0U;
    const pb2_proto_status_t st = pb2_frame_encode(hdr, payload, raw, sizeof(raw), &raw_len);
    if (st != PB2_PROTO_OK) {
        return st;
    }
    if (out_cap < 2U) {
        return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    }
    const size_t cobs_len = pb2_cobs_encode(raw, raw_len, out_wire, out_cap - 1U);
    if (cobs_len == 0U || cobs_len + 1U > out_cap) {
        return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    }
    out_wire[cobs_len] = 0x00U;
    *out_len = cobs_len + 1U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_hello(const pb2_hello_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 8U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    buf[0] = p->node_role;
    buf[1] = p->hw_rev;
    write_u16_le(buf + 2U, p->proto_max_payload);
    write_u32_le(buf + 4U, p->features_mask);
    *len = 8U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_hello(const uint8_t *buf, size_t len, pb2_hello_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 8U) return PB2_PROTO_ERR_TRUNCATED;
    p->node_role = buf[0];
    p->hw_rev = buf[1];
    p->proto_max_payload = read_u16_le(buf + 2U);
    p->features_mask = read_u32_le(buf + 4U);
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_capabilities(const pb2_capabilities_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 10U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, p->supported_rates_mask);
    write_u32_le(buf + 4U, p->max_baud_rate);
    buf[8] = p->adc_owner;
    buf[9] = p->channel_count;
    *len = 10U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_capabilities(const uint8_t *buf, size_t len, pb2_capabilities_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 10U) return PB2_PROTO_ERR_TRUNCATED;
    p->supported_rates_mask = read_u32_le(buf);
    p->max_baud_rate = read_u32_le(buf + 4U);
    p->adc_owner = buf[8];
    p->channel_count = buf[9];
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_heartbeat(const pb2_heartbeat_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 16U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u64_le(buf, p->uptime_ms);
    write_u32_le(buf + 8U, p->rx_frames);
    write_u32_le(buf + 12U, p->dropped_frames);
    *len = 16U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_heartbeat(const uint8_t *buf, size_t len, pb2_heartbeat_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 16U) return PB2_PROTO_ERR_TRUNCATED;
    p->uptime_ms = read_u64_le(buf);
    p->rx_frames = read_u32_le(buf + 8U);
    p->dropped_frames = read_u32_le(buf + 12U);
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_ping_pong(const pb2_ping_pong_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 13U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, p->nonce);
    write_u64_le(buf + 4U, p->timestamp_us);
    buf[12] = p->clock_domain;
    *len = 13U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_ping_pong(const uint8_t *buf, size_t len, pb2_ping_pong_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 13U) return PB2_PROTO_ERR_TRUNCATED;
    p->nonce = read_u32_le(buf);
    p->timestamp_us = read_u64_le(buf + 4U);
    p->clock_domain = buf[12];
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_ack(const pb2_ack_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 6U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, p->acked_sequence);
    buf[4] = p->acked_msg_type;
    buf[5] = p->status_code;
    *len = 6U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_ack(const uint8_t *buf, size_t len, pb2_ack_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 6U) return PB2_PROTO_ERR_TRUNCATED;
    p->acked_sequence = read_u32_le(buf);
    p->acked_msg_type = buf[4];
    p->status_code = buf[5];
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_error(const pb2_error_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 7U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, p->related_sequence);
    write_u16_le(buf + 4U, p->error_code);
    buf[6] = p->severity;
    *len = 7U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_error(const uint8_t *buf, size_t len, pb2_error_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 7U) return PB2_PROTO_ERR_TRUNCATED;
    p->related_sequence = read_u32_le(buf);
    p->error_code = read_u16_le(buf + 4U);
    p->severity = buf[6];
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_acq_status(const pb2_acq_status_payload_t *p, uint8_t *buf, size_t cap, uint16_t *len) {
    if (p == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 24U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    buf[0] = p->active;
    buf[1] = p->source_id;
    buf[2] = p->channel_count;
    buf[3] = p->sample_format;
    write_u32_le(buf + 4U, p->sample_rate_hz);
    write_u64_le(buf + 8U, p->total_frames);
    write_u64_le(buf + 16U, p->dropped_frames);
    *len = 24U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_acq_status(const uint8_t *buf, size_t len, pb2_acq_status_payload_t *p) {
    if (p == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 24U) return PB2_PROTO_ERR_TRUNCATED;
    p->active = buf[0];
    p->source_id = buf[1];
    p->channel_count = buf[2];
    p->sample_format = buf[3];
    p->sample_rate_hz = read_u32_le(buf + 4U);
    p->total_frames = read_u64_le(buf + 8U);
    p->dropped_frames = read_u64_le(buf + 16U);
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_preview_block(const pb2_preview_block_t *b, uint8_t *buf, size_t cap, uint16_t *len) {
    if (b == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (b->channel_count != PB2_PROTO_CHANNEL_COUNT || b->frame_count > PB2_PROTO_PREVIEW_MAX_FRAMES) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }
    const size_t needed = 33U + (size_t)b->frame_count * PB2_PROTO_CHANNEL_COUNT * 4U;
    if (cap < needed) {
        return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    }

    write_u64_le(buf, b->first_sample_index);
    write_u64_le(buf + 8U, b->first_timestamp_us);
    write_u32_le(buf + 16U, b->source_sample_rate_hz);
    write_u16_le(buf + 20U, b->decimation_factor);
    write_u16_le(buf + 22U, b->frame_count);
    write_u16_le(buf + 24U, b->block_flags);
    buf[26] = b->clock_domain;
    buf[27] = b->sample_format;
    buf[28] = b->channel_count;
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        buf[29U + ch] = b->channel_map[ch];
    }

    size_t offset = 33U;
    for (uint16_t f = 0U; f < b->frame_count; ++f) {
        for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
            write_f32_le(buf + offset, b->samples_uv[f][ch]);
            offset += 4U;
        }
    }
    *len = (uint16_t)needed;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_geometry_update(const pb2_geometry_update_t *g, uint8_t *buf, size_t cap, uint16_t *len) {
    if (g == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 64U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, g->session_epoch);
    buf[4] = g->geometry_source;
    buf[5] = g->sensor_count;
    buf[6] = g->valid;
    buf[7] = g->reserved;
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        buf[8U + ch] = g->channel_to_sensor[ch];
    }
    write_f32_le(buf + 12U, g->propagation_speed_mps);
    size_t offset = 16U;
    for (size_t s = 0U; s < PB2_PROTO_CHANNEL_COUNT; ++s) {
        for (size_t k = 0U; k < 3U; ++k) {
            write_f32_le(buf + offset, g->sensor_positions_m[s][k]);
            offset += 4U;
        }
    }
    *len = 64U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_geometry_update(const uint8_t *buf, size_t len, pb2_geometry_update_t *g) {
    if (g == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 64U) return PB2_PROTO_ERR_TRUNCATED;
    g->session_epoch = read_u32_le(buf);
    g->geometry_source = buf[4];
    g->sensor_count = buf[5];
    g->valid = buf[6];
    g->reserved = buf[7];
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        g->channel_to_sensor[ch] = buf[8U + ch];
    }
    g->propagation_speed_mps = read_f32_le(buf + 12U);
    size_t offset = 16U;
    for (size_t s = 0U; s < PB2_PROTO_CHANNEL_COUNT; ++s) {
        for (size_t k = 0U; k < 3U; ++k) {
            g->sensor_positions_m[s][k] = read_f32_le(buf + offset);
            offset += 4U;
        }
    }
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_encode_localization_result(const pb2_localization_result_t *r, uint8_t *buf, size_t cap, uint16_t *len) {
    if (r == NULL || buf == NULL || len == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (cap < 40U) return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
    write_u32_le(buf, r->session_epoch);
    buf[4] = r->valid;
    buf[5] = r->geometry_source;
    buf[6] = r->iterations;
    buf[7] = r->reserved;
    write_f32_le(buf + 8U, r->source_x_m);
    write_f32_le(buf + 12U, r->source_y_m);
    write_f32_le(buf + 16U, r->source_z_m);
    write_f32_le(buf + 20U, r->rms_residual_us);
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        write_u32_le(buf + 24U + ch * 4U, (uint32_t)r->tdoa_us[ch]);
    }
    *len = 40U;
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_localization_result(const uint8_t *buf, size_t len, pb2_localization_result_t *r) {
    if (r == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len != 40U) return PB2_PROTO_ERR_TRUNCATED;
    r->session_epoch = read_u32_le(buf);
    r->valid = buf[4];
    r->geometry_source = buf[5];
    r->iterations = buf[6];
    r->reserved = buf[7];
    r->source_x_m = read_f32_le(buf + 8U);
    r->source_y_m = read_f32_le(buf + 12U);
    r->source_z_m = read_f32_le(buf + 16U);
    r->rms_residual_us = read_f32_le(buf + 20U);
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        r->tdoa_us[ch] = (int32_t)read_u32_le(buf + 24U + ch * 4U);
    }
    return PB2_PROTO_OK;
}

pb2_proto_status_t pb2_decode_preview_block(const uint8_t *buf, size_t len, pb2_preview_block_t *b) {
    if (b == NULL || buf == NULL) return PB2_PROTO_ERR_INVALID_ARG;
    if (len < 33U) return PB2_PROTO_ERR_TRUNCATED;

    b->first_sample_index = read_u64_le(buf);
    b->first_timestamp_us = read_u64_le(buf + 8U);
    b->source_sample_rate_hz = read_u32_le(buf + 16U);
    b->decimation_factor = read_u16_le(buf + 20U);
    b->frame_count = read_u16_le(buf + 22U);
    b->block_flags = read_u16_le(buf + 24U);
    b->clock_domain = buf[26];
    b->sample_format = buf[27];
    b->channel_count = buf[28];
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        b->channel_map[ch] = buf[29U + ch];
    }

    if (b->channel_count != PB2_PROTO_CHANNEL_COUNT || b->frame_count > PB2_PROTO_PREVIEW_MAX_FRAMES) {
        return PB2_PROTO_ERR_OVERSIZED;
    }
    const size_t expected = 33U + (size_t)b->frame_count * PB2_PROTO_CHANNEL_COUNT * 4U;
    if (len != expected) {
        return PB2_PROTO_ERR_TRUNCATED;
    }

    size_t offset = 33U;
    for (uint16_t f = 0U; f < b->frame_count; ++f) {
        for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
            b->samples_uv[f][ch] = read_f32_le(buf + offset);
            offset += 4U;
        }
    }
    return PB2_PROTO_OK;
}

void pb2_stream_decoder_init(pb2_stream_decoder_t *dec) {
    if (dec == NULL) {
        return;
    }
    memset(dec, 0, sizeof(*dec));
}

void pb2_stream_decoder_feed(
    pb2_stream_decoder_t *dec,
    const uint8_t *bytes,
    size_t len,
    pb2_stream_frame_cb_t on_frame,
    void *user_ctx
) {
    if (dec == NULL || (bytes == NULL && len > 0U)) {
        return;
    }

    for (size_t i = 0U; i < len; ++i) {
        const uint8_t b = bytes[i];
        if (b == 0x00U) {
            if (dec->overflowed) {
                dec->overflowed = false;
                dec->rx_len = 0U;
                continue;
            }
            if (dec->rx_len == 0U) {
                continue;
            }
            uint8_t raw[PB2_PROTO_MAX_FRAME_BYTES];
            size_t raw_len = 0U;
            const pb2_proto_status_t cobs_st =
                pb2_cobs_decode(dec->rx_buf, dec->rx_len, raw, sizeof(raw), &raw_len);
            dec->rx_len = 0U;
            if (cobs_st != PB2_PROTO_OK) {
                if (cobs_st == PB2_PROTO_ERR_BUFFER_TOO_SMALL) {
                    ++dec->oversized_errors;
                } else {
                    ++dec->cobs_errors;
                }
                continue;
            }

            pb2_frame_header_t hdr;
            const uint8_t *payload = NULL;
            const pb2_proto_status_t frm_st = pb2_frame_decode(raw, raw_len, &hdr, &payload);
            if (frm_st == PB2_PROTO_ERR_BAD_CRC) {
                ++dec->crc_errors;
                continue;
            }
            if (frm_st != PB2_PROTO_OK) {
                ++dec->header_errors;
                continue;
            }

            ++dec->frames_ok;
            if (on_frame != NULL) {
                on_frame(&hdr, payload, user_ctx);
            }
        } else {
            if (dec->overflowed) {
                continue;
            }
            if (dec->rx_len >= sizeof(dec->rx_buf)) {
                dec->overflowed = true;
                dec->rx_len = 0U;
                ++dec->oversized_errors;
                continue;
            }
            dec->rx_buf[dec->rx_len++] = b;
        }
    }
}
