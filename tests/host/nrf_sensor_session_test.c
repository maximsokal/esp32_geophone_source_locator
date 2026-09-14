#include <assert.h>
#include <string.h>

#include "nrf_sensor_session.h"

static nrf_upstream_assign_t make_remote_assign(uint8_t node_id, uint8_t channel)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = 1U;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 1U;
    assign.hardware_id[1] = 2U;
    assign.hardware_id[2] = 3U;
    assign.hardware_id[3] = 4U;
    assign.role = NRF_UPSTREAM_ROLE_REMOTE;
    assign.node_id = node_id;
    assign.mux_channel = channel;
    return assign;
}

static nrf_upstream_sensor_snapshot_t make_final(uint32_t request_id, uint32_t sequence)
{
    nrf_upstream_sensor_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.request_id = request_id;
    snapshot.sequence = sequence;
    snapshot.valid_mask = NRF_UPSTREAM_SENSOR_VALID_IMU;
    snapshot.fault_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
    snapshot.result = NRF_UPSTREAM_SENSOR_RESULT_PARTIAL;
    snapshot.flags = NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED |
                     NRF_UPSTREAM_SENSOR_FLAG_IMU_READY |
                     NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
    snapshot.accel_mg[2] = 1000;
    return snapshot;
}

static void test_inactive_request_is_rejected_without_publication_change(void)
{
    nrf_sensor_session_t session;
    nrf_upstream_sensor_snapshot_t before;
    nrf_upstream_sensor_snapshot_t after;
    nrf_sensor_session_init(&session);
    nrf_sensor_session_get_published(&session, &before);
    assert(nrf_sensor_session_begin_request(&session, 10U) == NRF_SENSOR_SESSION_REQUEST_NOT_REMOTE);
    nrf_sensor_session_get_published(&session, &after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_activation_is_idempotent_and_initial_sample_is_pending(void)
{
    nrf_sensor_session_t session;
    nrf_upstream_assign_t assign = make_remote_assign(3U, 2U);
    nrf_upstream_assign_t moved = make_remote_assign(4U, 3U);
    nrf_upstream_sensor_snapshot_t published;

    nrf_sensor_session_init(&session);
    assert(nrf_sensor_session_activate_remote(&session, &assign) == NRF_SENSOR_SESSION_ACTIVATE_NEW);
    nrf_sensor_session_get_published(&session, &published);
    assert(published.request_id == 0U);
    assert(published.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(nrf_sensor_session_begin_request(&session, 10U) == NRF_SENSOR_SESSION_REQUEST_BUSY);
    assert(nrf_sensor_session_activate_remote(&session, &assign) == NRF_SENSOR_SESSION_ACTIVATE_IDEMPOTENT);
    assert(nrf_sensor_session_activate_remote(&session, &moved) == NRF_SENSOR_SESSION_ACTIVATE_CONFLICT);
}

static void test_initial_completion_then_explicit_idempotency(void)
{
    nrf_sensor_session_t session;
    nrf_upstream_assign_t assign = make_remote_assign(2U, 1U);
    nrf_upstream_sensor_snapshot_t initial = make_final(0U, 1U);
    nrf_upstream_sensor_snapshot_t final = make_final(55U, 2U);
    nrf_upstream_sensor_snapshot_t published;

    nrf_sensor_session_init(&session);
    assert(nrf_sensor_session_activate_remote(&session, &assign) == NRF_SENSOR_SESSION_ACTIVATE_NEW);
    assert(nrf_sensor_session_complete(&session, 0U, &initial));
    assert(nrf_sensor_session_begin_request(&session, 55U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
    assert(nrf_sensor_session_begin_request(&session, 55U) == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT);
    assert(nrf_sensor_session_begin_request(&session, 56U) == NRF_SENSOR_SESSION_REQUEST_BUSY);
    nrf_sensor_session_get_published(&session, &published);
    assert(published.request_id == 55U && published.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(published.sequence == 1U);
    assert((published.flags & NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE) == 0U);

    assert(nrf_sensor_session_complete(&session, 55U, &final));
    assert(nrf_sensor_session_begin_request(&session, 55U) == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT);
    nrf_sensor_session_get_published(&session, &published);
    assert(memcmp(&published, &final, sizeof(final)) == 0);

    assert(nrf_sensor_session_begin_request(&session, 56U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
}

static void test_queue_failure_becomes_stable_busy_result(void)
{
    nrf_sensor_session_t session;
    nrf_upstream_assign_t assign = make_remote_assign(2U, 1U);
    nrf_upstream_sensor_snapshot_t initial = make_final(0U, 1U);
    nrf_upstream_sensor_snapshot_t published;

    nrf_sensor_session_init(&session);
    assert(nrf_sensor_session_activate_remote(&session, &assign) == NRF_SENSOR_SESSION_ACTIVATE_NEW);
    assert(nrf_sensor_session_complete(&session, 0U, &initial));
    assert(nrf_sensor_session_begin_request(&session, 99U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
    assert(nrf_sensor_session_mark_enqueue_busy(&session, 99U));
    nrf_sensor_session_get_published(&session, &published);
    assert(published.request_id == 99U);
    assert(published.result == NRF_UPSTREAM_SENSOR_RESULT_BUSY);
    assert(nrf_sensor_session_begin_request(&session, 99U) == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT);
    assert(nrf_sensor_session_begin_request(&session, 100U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
}

int main(void)
{
    test_inactive_request_is_rejected_without_publication_change();
    test_activation_is_idempotent_and_initial_sample_is_pending();
    test_initial_completion_then_explicit_idempotency();
    test_queue_failure_becomes_stable_busy_result();
    return 0;
}
