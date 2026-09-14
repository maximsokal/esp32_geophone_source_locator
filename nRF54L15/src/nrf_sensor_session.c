#include "nrf_sensor_session.h"

#include <string.h>

static uint8_t readiness_flags(uint8_t flags)
{
    return flags & (NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED |
                    NRF_UPSTREAM_SENSOR_FLAG_IMU_READY |
                    NRF_UPSTREAM_SENSOR_FLAG_ENV_READY);
}

void nrf_sensor_session_init(nrf_sensor_session_t *session)
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->published.result = NRF_UPSTREAM_SENSOR_RESULT_NOT_READY;
}

nrf_sensor_session_activate_result_t nrf_sensor_session_activate_remote(
    nrf_sensor_session_t *session,
    const nrf_upstream_assign_t *assign)
{
    if (session == NULL || assign == NULL ||
        nrf_upstream_protocol_validate_assign(assign) != NRF_UPSTREAM_RESULT_OK ||
        assign->role != NRF_UPSTREAM_ROLE_REMOTE) {
        return NRF_SENSOR_SESSION_ACTIVATE_INVALID;
    }

    if (session->remote_active) {
        return session->node_id == assign->node_id &&
                       session->mux_channel == assign->mux_channel
                   ? NRF_SENSOR_SESSION_ACTIVATE_IDEMPOTENT
                   : NRF_SENSOR_SESSION_ACTIVATE_CONFLICT;
    }

    session->remote_active = true;
    session->node_id = assign->node_id;
    session->mux_channel = assign->mux_channel;
    session->request_pending = true;
    session->active_request_id = 0U;
    session->completed_request_valid = false;
    session->completed_request_id = 0U;
    memset(&session->published, 0, sizeof(session->published));
    session->published.result = NRF_UPSTREAM_SENSOR_RESULT_PENDING;
    return NRF_SENSOR_SESSION_ACTIVATE_NEW;
}

nrf_sensor_session_request_result_t nrf_sensor_session_begin_request(
    nrf_sensor_session_t *session,
    uint32_t request_id)
{
    uint8_t flags;
    uint32_t sequence;

    if (session == NULL || request_id == 0U) {
        return NRF_SENSOR_SESSION_REQUEST_INVALID;
    }
    if (!session->remote_active) {
        return NRF_SENSOR_SESSION_REQUEST_NOT_REMOTE;
    }
    if (session->request_pending) {
        return session->active_request_id == request_id
                   ? NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT
                   : NRF_SENSOR_SESSION_REQUEST_BUSY;
    }
    if (session->completed_request_valid &&
        session->completed_request_id == request_id) {
        return NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT;
    }

    flags = readiness_flags(session->published.flags);
    sequence = session->published.sequence;
    memset(&session->published, 0, sizeof(session->published));
    session->published.request_id = request_id;
    session->published.sequence = sequence;
    session->published.result = NRF_UPSTREAM_SENSOR_RESULT_PENDING;
    session->published.flags = flags;
    session->request_pending = true;
    session->active_request_id = request_id;
    return NRF_SENSOR_SESSION_REQUEST_ACCEPTED;
}

bool nrf_sensor_session_complete(
    nrf_sensor_session_t *session,
    uint32_t request_id,
    const nrf_upstream_sensor_snapshot_t *snapshot)
{
    if (session == NULL || snapshot == NULL || !session->remote_active ||
        !session->request_pending || session->active_request_id != request_id ||
        snapshot->request_id != request_id ||
        snapshot->result == NRF_UPSTREAM_SENSOR_RESULT_PENDING) {
        return false;
    }

    session->published = *snapshot;
    session->request_pending = false;
    session->active_request_id = 0U;
    if (request_id != 0U) {
        session->completed_request_valid = true;
        session->completed_request_id = request_id;
    }
    return true;
}

bool nrf_sensor_session_mark_enqueue_busy(
    nrf_sensor_session_t *session,
    uint32_t request_id)
{
    if (session == NULL || request_id == 0U || !session->request_pending ||
        session->active_request_id != request_id) {
        return false;
    }

    session->published.result = NRF_UPSTREAM_SENSOR_RESULT_BUSY;
    session->request_pending = false;
    session->active_request_id = 0U;
    session->completed_request_valid = true;
    session->completed_request_id = request_id;
    return true;
}

void nrf_sensor_session_get_published(
    const nrf_sensor_session_t *session,
    nrf_upstream_sensor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (session == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
        return;
    }
    *snapshot = session->published;
}
