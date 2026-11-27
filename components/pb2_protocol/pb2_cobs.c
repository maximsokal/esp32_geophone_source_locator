#include "pb2_protocol.h"

size_t pb2_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    if (out == NULL || (in == NULL && in_len > 0U)) {
        return 0U;
    }

    size_t read_idx = 0U;
    size_t write_idx = 1U;
    size_t code_idx = 0U;
    uint8_t code = 1U;

    if (out_cap < 1U) {
        return 0U;
    }

    while (read_idx < in_len) {
        if (in[read_idx] == 0U) {
            out[code_idx] = code;
            code = 1U;
            code_idx = write_idx++;
            if (write_idx > out_cap) {
                return 0U;
            }
            ++read_idx;
        } else {
            if (write_idx >= out_cap) {
                return 0U;
            }
            out[write_idx++] = in[read_idx++];
            ++code;
            if (code == 0xFFU) {
                out[code_idx] = code;
                code = 1U;
                code_idx = write_idx++;
                if (write_idx > out_cap && read_idx < in_len) {
                    return 0U;
                }
            }
        }
    }

    if (code_idx >= out_cap) {
        return 0U;
    }
    out[code_idx] = code;
    return write_idx;
}

pb2_proto_status_t pb2_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (out == NULL || out_len == NULL || (in == NULL && in_len > 0U)) {
        return PB2_PROTO_ERR_INVALID_ARG;
    }

    size_t read_idx = 0U;
    size_t write_idx = 0U;

    while (read_idx < in_len) {
        const uint8_t code = in[read_idx];
        if (code == 0U) {
            return PB2_PROTO_ERR_COBS_DECODE;
        }
        ++read_idx;

        const size_t copy_count = (size_t)code - 1U;
        if (read_idx + copy_count > in_len) {
            return PB2_PROTO_ERR_COBS_DECODE;
        }
        if (write_idx + copy_count > out_cap) {
            return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
        }

        for (size_t i = 0U; i < copy_count; ++i) {
            if (in[read_idx] == 0U) {
                return PB2_PROTO_ERR_COBS_DECODE;
            }
            out[write_idx++] = in[read_idx++];
        }

        if (code < 0xFFU && read_idx < in_len) {
            if (write_idx >= out_cap) {
                return PB2_PROTO_ERR_BUFFER_TOO_SMALL;
            }
            out[write_idx++] = 0U;
        }
    }

    *out_len = write_idx;
    return PB2_PROTO_OK;
}
