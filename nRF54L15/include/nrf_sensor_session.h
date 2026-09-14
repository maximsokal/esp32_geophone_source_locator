#ifndef NRF_SENSOR_SESSION_H
#define NRF_SENSOR_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_SENSOR_SESSION_ACTIVATE_NEW = 0,
    NRF_SENSOR_SESSION_ACTIVATE_IDEMPOTENT = 1,
    NRF_SENSOR_SESSION_ACTIVATE_CONFLICT = 2,
    NRF_SENSOR_SESSION_ACTIVATE_INVALID = -1,
} nrf_sensor_session_activate_result_t;

typedef enum {
    NRF_SENSOR_SESSION_REQUEST_ACCEPTED = 0,
    NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT = 1,
    NRF_SENSOR_SESSION_REQUEST_BUSY = 2,
    NRF_SENSOR_SESSION_REQUEST_NOT_REMOTE = 3,
    NRF_SENSOR_SESSION_REQUEST_INVALID = -1,
} nrf_sensor_session_request_result_t;

typedef struct {
    bool remote_active;
    uint8_t node_id;
    uint8_t mux_channel;
    bool request_pending;
    uint32_t active_request_id;
    bool completed_request_valid;
    uint32_t completed_request_id;
    nrf_upstream_sensor_snapshot_t published;
} nrf_sensor_session_t;

void nrf_sensor_session_init(nrf_sensor_session_t *session);

nrf_sensor_session_activate_result_t nrf_sensor_session_activate_remote(
    nrf_sensor_session_t *session,
    const nrf_upstream_assign_t *assign);

nrf_sensor_session_request_result_t nrf_sensor_session_begin_request(
    nrf_sensor_session_t *session,
    uint32_t request_id);

bool nrf_sensor_session_complete(
    nrf_sensor_session_t *session,
    uint32_t request_id,
    const nrf_upstream_sensor_snapshot_t *snapshot);

bool nrf_sensor_session_mark_enqueue_busy(
    nrf_sensor_session_t *session,
    uint32_t request_id);

void nrf_sensor_session_get_published(
    const nrf_sensor_session_t *session,
    nrf_upstream_sensor_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
