#ifndef NRF_READ_WINDOW_H
#define NRF_READ_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Windowed reads of responses longer than one FIFO transfer.
 *
 * The ESP32 master splits a read of more than 31 bytes into FIFO segments and
 * holds SCL low between them. The 2026-10-26 logic analyzer capture of two
 * REMOTE nodes shows every such read damaged in its tail (CRC wrong on 96 of
 * 138 frames longer than 31 bytes) while all 259 reads of 28 bytes were intact.
 * Long responses are therefore fetched as consecutive windows of at most
 * NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES bytes:
 *
 *   read   [0, 28)                          the ordinary read, selects the response
 *   write  READ_WINDOW(offset = 28)
 *   read   [28, 56)                         served from the frozen copy
 *   ...
 *
 * The target keeps a copy of the last response it handed out, so the windows
 * always belong to the one response that the first read selected. The caller
 * verifies the assembled frame with its own CRC.
 */
#define NRF_READ_WINDOW_CAPACITY NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH
#define NRF_READ_WINDOW_ERR_ARGUMENT INT32_MIN

/* ---- target side (nRF, TWIS callback context) -------------------------- */

typedef struct {
    uint8_t frame[NRF_READ_WINDOW_CAPACITY];
    uint16_t length;
    uint16_t offset;
    bool armed;
} nrf_read_window_target_t;

void nrf_read_window_target_init(nrf_read_window_target_t *target);

/*
 * Remember the response that was just handed to the master. A NULL frame or a
 * length of zero or above NRF_READ_WINDOW_CAPACITY forgets it (nothing can be
 * windowed). Also disarms any pending window.
 */
void nrf_read_window_target_record(
    nrf_read_window_target_t *target,
    const uint8_t *frame,
    size_t length);

/*
 * Select where the next read starts. Returns false (and stays disarmed) when no
 * response is recorded or offset is not inside it.
 */
bool nrf_read_window_target_arm(nrf_read_window_target_t *target, uint16_t offset);

/* A write that is not a window request must not leave a window armed. */
void nrf_read_window_target_disarm(nrf_read_window_target_t *target);

/*
 * If a window is armed, point at the bytes from the armed offset to the end of
 * the recorded response, disarm, and return true. The recorded response itself
 * is kept, so further windows can follow.
 */
bool nrf_read_window_target_serve(
    nrf_read_window_target_t *target,
    const uint8_t **data,
    uint32_t *length);

/* ---- controller side (ESP32) ------------------------------------------- */

typedef struct {
    void *context;
    /* Both return 0 on success and the platform's error code otherwise. */
    int (*write_window)(void *context, const uint8_t frame[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH]);
    int (*read)(void *context, uint8_t *data, size_t length);
} nrf_read_window_io_t;

/*
 * Read `length` bytes of the response selected by the preceding request.
 * Up to one window this is a single plain read, so short responses behave as
 * before. Returns 0, the first error an io callback returned, or
 * NRF_READ_WINDOW_ERR_ARGUMENT. On error `frame` holds a partial response.
 */
int nrf_read_window_client_read(
    const nrf_read_window_io_t *io,
    uint8_t *frame,
    size_t length);

#ifdef __cplusplus
}
#endif

#endif
