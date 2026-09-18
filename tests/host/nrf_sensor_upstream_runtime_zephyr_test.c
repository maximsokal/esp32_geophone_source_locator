#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_session.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"

static unsigned s_thread_create_calls;
static unsigned s_queue_put_calls;
static unsigned s_prepare_calls;
static unsigned s_refresh_calls;
static unsigned s_imu_read_calls;
static unsigned s_runtime_init_calls;
static bool s_fail_next_queue_put;
static bool s_exit_worker_when_empty;
static bool s_include_env;
static jmp_buf s_worker_exit;
static void (*s_worker_entry)(void *, void *, void *);

void k_msgq_purge(struct k_msgq *q)
{
    q->head = q->tail = q->count = 0U;
}

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout)
{
    (void)timeout;
    ++s_queue_put_calls;
    if (s_fail_next_queue_put) {
        s_fail_next_queue_put = false;
        return -ENOMSG;
    }
    if (q->count >= q->max_msgs) {
        return -ENOMSG;
    }
    memcpy(q->storage[q->tail], data, q->msg_size);
    q->tail = (q->tail + 1U) % q->max_msgs;
    ++q->count;
    return 0;
}

int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout)
{
    (void)timeout;
    if (q->count == 0U) {
        if (s_exit_worker_when_empty) {
            longjmp(s_worker_exit, 1);
        }
        return -ENOMSG;
    }
    memcpy(data, q->storage[q->head], q->msg_size);
    q->head = (q->head + 1U) % q->max_msgs;
    --q->count;
    return 0;
}

k_tid_t k_thread_create(
    struct k_thread *thread,
    void *stack,
    size_t stack_size,
    void (*entry)(void *, void *, void *),
    void *p1,
    void *p2,
    void *p3,
    int prio,
    uint32_t options,
    k_timeout_t delay)
{
    (void)stack;
    (void)stack_size;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)prio;
    (void)options;
    (void)delay;
    ++s_thread_create_calls;
    s_worker_entry = entry;
    return thread;
}

int64_t k_uptime_get(void)
{
    return 1234;
}

void nrf_node_state_init(nrf_node_state_t *state)
{
    state->role = NRF_NODE_ROLE_UNASSIGNED;
    state->lifecycle = NRF_NODE_LIFECYCLE_BOOTING;
}

void nrf_node_state_reset(nrf_node_state_t *state)
{
    nrf_node_state_init(state);
}

nrf_node_state_result_t nrf_node_state_mark_ready(nrf_node_state_t *state)
{
    state->lifecycle = NRF_NODE_LIFECYCLE_READY;
    return NRF_NODE_STATE_OK;
}

nrf_node_state_result_t nrf_node_state_assign_role(nrf_node_state_t *state, nrf_node_role_t role)
{
    state->role = role;
    return NRF_NODE_STATE_OK;
}

nrf_node_state_result_t nrf_node_state_mark_unavailable(nrf_node_state_t *state)
{
    state->lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
    return NRF_NODE_STATE_OK;
}

nrf_node_state_result_t nrf_node_state_mark_available(nrf_node_state_t *state)
{
    state->lifecycle = NRF_NODE_LIFECYCLE_READY;
    return NRF_NODE_STATE_OK;
}

static int fake_read_imu(void *context, nrf_imu_sample_t *sample)
{
    (void)context;
    ++s_imu_read_calls;
    memset(sample, 0, sizeof(*sample));
    sample->accel_mg[2] = 1000;
    return 0;
}

void nrf_remote_sensor_runtime_zephyr_init(nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    ++s_runtime_init_calls;
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    (void)node_state;
    ++s_prepare_calls;
    runtime->core.imu_ready = true;
    runtime->core.env_ready = s_include_env;
    runtime->core.sources.read_imu = fake_read_imu;
    runtime->core.sources.imu_context = NULL;
    return NRF_REMOTE_SENSOR_PREPARE_PARTIAL;
}

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    (void)node_state;
    ++s_refresh_calls;
    runtime->core.snapshot.valid_mask = NRF_SENSOR_VALID_IMU;
    if (s_include_env) {
        runtime->core.snapshot.valid_mask |= NRF_SENSOR_VALID_ENV;
        runtime->core.snapshot.env.temperature_milli_c = 21500;
        runtime->core.snapshot.env.pressure_pa = 101205U;
        runtime->core.snapshot.env.humidity_milli_percent = 50300U;
    }
    return fake_read_imu(NULL, &runtime->core.snapshot.imu) == 0
        ? NRF_SENSOR_SNAPSHOT_PARTIAL
        : NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
}

static nrf_upstream_assign_t make_assign(void)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = 10U;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 1U;
    assign.hardware_id[1] = 2U;
    assign.hardware_id[2] = 3U;
    assign.hardware_id[3] = 4U;
    assign.role = NRF_UPSTREAM_ROLE_REMOTE;
    assign.node_id = 2U;
    assign.mux_channel = 1U;
    return assign;
}

int main(void)
{
    nrf_orientation_config_t orientation_config;
    nrf_orientation_output_t orientation;
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_assign_t conflicting = assign;
    nrf_upstream_sensor_refresh_t request = {.request_id = 20U};
    nrf_upstream_sensor_snapshot_t snapshot;

    nrf_orientation_default_config(&orientation_config);
    orientation_config.reference_axis = NRF_ORIENTATION_AXIS_POS_Z;
    assert(nrf_sensor_upstream_runtime_zephyr_configure_orientation(
               &orientation_config) == 0);

    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    assert(s_thread_create_calls == 1U);
    assert(s_worker_entry != NULL);
    assert(s_runtime_init_calls == 1U);
    assert(s_prepare_calls == 0U && s_refresh_calls == 0U && s_imu_read_calls == 0U);
    assert(nrf_sensor_upstream_runtime_zephyr_configure_orientation(
               &orientation_config) == -EBUSY);

    assert(nrf_sensor_upstream_runtime_zephyr_get_orientation(&orientation));
    assert(orientation.state == NRF_ORIENTATION_STATE_UNAVAILABLE);
    assert(orientation.reason == NRF_ORIENTATION_REASON_SAMPLE_INVALID);

    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_NOT_READY);

    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == -EAGAIN);
    assert(s_queue_put_calls == 0U);
    assert(s_prepare_calls == 0U && s_refresh_calls == 0U && s_imu_read_calls == 0U);

    /* Failed ACTIVATE enqueue rolls the lightweight session activation back. */
    s_fail_next_queue_put = true;
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == -ENOSPC);
    assert(s_queue_put_calls == 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_NOT_READY);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == -EAGAIN);
    assert(s_queue_put_calls == 1U);

    /*
     * Successful notification commits logical REMOTE activation synchronously,
     * before the worker performs any private-sensor I/O. An immediate external
     * refresh therefore observes request_id=0 PENDING/BUSY, never NOT_REMOTE.
     */
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    assert(s_queue_put_calls == 2U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U);
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == -EBUSY);
    assert(s_queue_put_calls == 2U);
    assert(s_prepare_calls == 0U && s_refresh_calls == 0U && s_imu_read_calls == 0U);

    /* Execute exactly the queued ACTIVATE event, then escape the worker loop. */
    s_exit_worker_when_empty = true;
    if (setjmp(s_worker_exit) == 0) {
        s_worker_entry(NULL, NULL, NULL);
        assert(false);
    }
    s_exit_worker_when_empty = false;

    assert(s_prepare_calls == 1U);
    assert(s_refresh_calls == 1U);
    assert(s_imu_read_calls == 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U);
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);

    /*
     * The fake refresh only marks IMU valid (no ENV), so altitude_mm must
     * stay at its zeroed default; the wire snapshot's orientation fields
     * must reflect the same, freshly-updated estimator state a direct
     * get_orientation() call observes right after this refresh (accel
     * purely on +Z, configured reference axis, exactly 1g -> ACCEPTED).
     */
    assert(snapshot.altitude_mm == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_get_orientation(&orientation));
    assert(orientation.state == NRF_ORIENTATION_STATE_ACCEPTED);
    assert(snapshot.orientation_state == (uint8_t)orientation.state);
    assert(snapshot.orientation_blink_class == (uint8_t)orientation.blink_class);
    assert(snapshot.orientation_tilt_sin2_ppm == orientation.tilt_sin2_ppm);

    /* The explicit request is accepted after the activation sample completes. */
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    assert(s_queue_put_calls == 3U);

    /* A later aggregate refresh with a valid ENV sample must carry the
     * nRF-computed altitude alongside the legacy pressure/humidity fields. */
    s_include_env = true;
    s_exit_worker_when_empty = true;
    if (setjmp(s_worker_exit) == 0) {
        s_worker_entry(NULL, NULL, NULL);
        assert(false);
    }
    s_exit_worker_when_empty = false;
    assert(s_refresh_calls == 2U);
    assert(s_imu_read_calls == 2U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == request.request_id);
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
    assert((snapshot.valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U);
    assert(snapshot.pressure_pa == 101205U);
    assert(snapshot.altitude_mm == 10000);
    assert(snapshot.flags & NRF_UPSTREAM_SENSOR_FLAG_ENV_READY);
    assert(snapshot.flags & NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE);

    /* Same assignment is idempotent; a conflicting active identity is rejected. */
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    assert(s_queue_put_calls == 3U);
    conflicting.node_id = 3U;
    conflicting.mux_channel = 2U;
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&conflicting) == -EALREADY);
    assert(s_queue_put_calls == 3U);
    return 0;
}
