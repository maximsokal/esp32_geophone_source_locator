#include "nrf_read_window.h"

#include <string.h>

_Static_assert(
    NRF_READ_WINDOW_CAPACITY >= 136U,
    "the window buffer must hold the largest response (snapshot V3 / log block)");

void nrf_read_window_target_init(nrf_read_window_target_t *target)
{
    if (target != NULL) {
        memset(target, 0, sizeof(*target));
    }
}

void nrf_read_window_target_record(
    nrf_read_window_target_t *target,
    const uint8_t *frame,
    size_t length)
{
    if (target == NULL) {
        return;
    }
    target->armed = false;
    target->offset = 0U;
    if (frame == NULL || length == 0U || length > NRF_READ_WINDOW_CAPACITY) {
        target->length = 0U;
        return;
    }
    memcpy(target->frame, frame, length);
    target->length = (uint16_t)length;
}

bool nrf_read_window_target_arm(nrf_read_window_target_t *target, uint16_t offset)
{
    if (target == NULL) {
        return false;
    }
    if (target->length == 0U || offset >= target->length) {
        target->armed = false;
        return false;
    }
    target->offset = offset;
    target->armed = true;
    return true;
}

void nrf_read_window_target_disarm(nrf_read_window_target_t *target)
{
    if (target != NULL) {
        target->armed = false;
    }
}

bool nrf_read_window_target_serve(
    nrf_read_window_target_t *target,
    const uint8_t **data,
    uint32_t *length)
{
    if (target == NULL || data == NULL || length == NULL || !target->armed) {
        return false;
    }
    target->armed = false;
    if (target->offset >= target->length) {
        return false;
    }
    *data = &target->frame[target->offset];
    *length = (uint32_t)(target->length - target->offset);
    return true;
}

int nrf_read_window_client_read(
    const nrf_read_window_io_t *io,
    uint8_t *frame,
    size_t length)
{
    size_t offset = 0U;

    if (io == NULL || io->read == NULL || io->write_window == NULL ||
        frame == NULL || length == 0U || length > NRF_READ_WINDOW_CAPACITY) {
        return NRF_READ_WINDOW_ERR_ARGUMENT;
    }

    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t chunk = remaining < NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES
            ? remaining
            : NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
        int result;

        if (offset != 0U) {
            uint8_t request[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
            const nrf_upstream_read_window_t window = {.offset = (uint16_t)offset};

            if (nrf_upstream_protocol_encode_read_window(&window, request) !=
                NRF_UPSTREAM_RESULT_OK) {
                return NRF_READ_WINDOW_ERR_ARGUMENT;
            }
            result = io->write_window(io->context, request);
            if (result != 0) {
                return result;
            }
        }
        result = io->read(io->context, &frame[offset], chunk);
        if (result != 0) {
            return result;
        }
        offset += chunk;
    }
    return 0;
}
