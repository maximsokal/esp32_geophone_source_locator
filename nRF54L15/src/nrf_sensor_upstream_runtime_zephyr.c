#include "nrf_sensor_upstream_runtime_zephyr.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_node_state.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_session.h"

#define NRF_SENSOR_EVENT_QUEUE_DEPTH 4U
#define NRF_SENSOR_WORKER_STACK_SIZE 3072U
#define NRF_SENSOR_WORKER_PRIORITY K_PRIO_PREEMPT(10)

typedef enum {
    NRF_SENSOR_EVENT_ACTIVATE = 1,
    NRF_SENSOR_EVENT_REFRESH = 2,
} nrf_sensor_event_type_t;

typedef struct {
    uint8_t type;
    uint32_t request_id;
    nrf_upstream_assign_t assign;
} nrf_sensor_event_t;

K_MSGQ_DEFINE(
    s_sensor_queue,
    sizeof(nrf_sensor_event_t),
    NRF_SENSOR_EVENT_QUEUE_DEPTH,
    4);
K_THREAD_STACK_DEFINE(s_sensor_stack, NRF_SENSOR_WORKER_STACK_SIZE);

static struct k_thread s_sensor_thread;
static struct k_spinlock s_session_lock;
static struct k_spinlock s_orientation_lock;
static nrf_sensor_session_t s_session;
static nrf_remote_sensor_runtime_zephyr_t s_runtime;
static nrf_node_state_t s_worker_node_state;
static nrf_orientation_config_t s_orientation_config;
static nrf_orientation_estimator_t s_orientation_estimator;
static uint64_t s_next_orientation_due_ms;
static bool s_orientation_config_set;
static bool s_started;
static bool s_activation_queued;

static uint64_t runtime_uptime_ms(void)
{
    const int64_t uptime = k_uptime_get();
    return uptime > 0 ? (uint64_t)uptime : 0U;
}

static uint8_t runtime_flags(void)
{
    uint8_t flags = NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED;
    if (s_runtime.core.imu_ready) {
        flags |= NRF_UPSTREAM_SENSOR_FLAG_IMU_READY;
    }
    if (s_runtime.core.env_ready) {
        flags |= NRF_UPSTREAM_SENSOR_FLAG_ENV_READY;
    }
    return flags;
}

static uint8_t map_snapshot_result(nrf_sensor_snapshot_result_t result)
{
    switch (result) {
    case NRF_SENSOR_SNAPSHOT_OK:
        return NRF_UPSTREAM_SENSOR_RESULT_OK;
    case NRF_SENSOR_SNAPSHOT_PARTIAL:
        return NRF_UPSTREAM_SENSOR_RESULT_PARTIAL;
    case NRF_SENSOR_SNAPSHOT_SKIPPED:
        return NRF_UPSTREAM_SENSOR_RESULT_NOT_REMOTE;
    case NRF_SENSOR_SNAPSHOT_ERR_NOT_READY:
        return NRF_UPSTREAM_SENSOR_RESULT_NOT_READY;
    case NRF_SENSOR_SNAPSHOT_ERR_NO_DATA:
        return NRF_UPSTREAM_SENSOR_RESULT_NO_DATA;
    case NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT:
    default:
        return NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
    }
}

static void copy_runtime_payload(
    uint32_t request_id,
    bool did_refresh,
    nrf_sensor_snapshot_result_t refresh_result,
    uint64_t completion_uptime_ms,
    nrf_upstream_sensor_snapshot_t *wire)
{
    const nrf_sensor_snapshot_t *snapshot = &s_runtime.core.snapshot;

    memset(wire, 0, sizeof(*wire));
    wire->request_id = request_id;
    wire->sequence = snapshot->sequence;
    wire->valid_mask = snapshot->valid_mask;
    wire->fault_mask = snapshot->fault_mask;
    if (!did_refresh) {
        wire->valid_mask = 0U;
        wire->fault_mask = 0U;
        if (!s_runtime.core.imu_ready) {
            wire->fault_mask |= NRF_UPSTREAM_SENSOR_VALID_IMU;
        }
        if (!s_runtime.core.env_ready) {
            wire->fault_mask |= NRF_UPSTREAM_SENSOR_VALID_ENV;
        }
    }
    wire->sample_complete_uptime_ms = completion_uptime_ms;
    wire->result = did_refresh
        ? map_snapshot_result(refresh_result)
        : NRF_UPSTREAM_SENSOR_RESULT_NO_DATA;
    wire->flags = runtime_flags();

    if ((snapshot->valid_mask & NRF_SENSOR_VALID_IMU) != 0U) {
        wire->accel_mg[0] = snapshot->imu.accel_mg[0];
        wire->accel_mg[1] = snapshot->imu.accel_mg[1];
        wire->accel_mg[2] = snapshot->imu.accel_mg[2];
        wire->gyro_mdps[0] = snapshot->imu.gyro_mdps[0];
        wire->gyro_mdps[1] = snapshot->imu.gyro_mdps[1];
        wire->gyro_mdps[2] = snapshot->imu.gyro_mdps[2];
        wire->imu_temperature_milli_c = snapshot->imu.temperature_milli_c;
    }
    if ((snapshot->valid_mask & NRF_SENSOR_VALID_ENV) != 0U) {
        wire->env_temperature_milli_c = snapshot->env.temperature_milli_c;
        wire->pressure_pa = snapshot->env.pressure_pa;
        wire->humidity_milli_percent = snapshot->env.humidity_milli_percent;
    }
    if (wire->valid_mask != 0U) {
        wire->flags |= NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
    }
}

static void orientation_advance_deadline(uint64_t sample_uptime_ms)
{
    const uint64_t period = s_orientation_config.sample_period_ms;
    const uint64_t target = sample_uptime_ms + period;

    if (period == 0U) {
        s_next_orientation_due_ms = 0U;
        return;
    }
    if (s_next_orientation_due_ms == 0U) {
        s_next_orientation_due_ms = target;
        return;
    }
    if (s_next_orientation_due_ms < target) {
        const uint64_t delta = target - s_next_orientation_due_ms;
        const uint64_t steps = (delta + period - 1U) / period;
        s_next_orientation_due_ms += steps * period;
    }
}

static void orientation_reset(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_orientation_lock);
    (void)nrf_orientation_estimator_init(
        &s_orientation_estimator,
        &s_orientation_config);
    k_spin_unlock(&s_orientation_lock, key);
    s_next_orientation_due_ms = 0U;
}

static void orientation_feed_sample(
    const nrf_imu_sample_t *sample,
    bool sample_valid,
    uint64_t sample_uptime_ms)
{
    nrf_imu_sample_t empty = {0};
    k_spinlock_key_t key;

    if (sample == NULL) {
        sample = &empty;
    }
    key = k_spin_lock(&s_orientation_lock);
    (void)nrf_orientation_estimator_update(
        &s_orientation_estimator,
        sample,
        sample_valid);
    k_spin_unlock(&s_orientation_lock, key);
    orientation_advance_deadline(sample_uptime_ms);
}

static bool orientation_periodic_enabled(void)
{
    return s_orientation_config.reference_axis != NRF_ORIENTATION_AXIS_UNCONFIGURED &&
           s_worker_node_state.lifecycle == NRF_NODE_LIFECYCLE_READY &&
           s_worker_node_state.role == NRF_NODE_ROLE_REMOTE &&
           s_runtime.core.imu_ready &&
           s_runtime.core.sources.read_imu != NULL;
}

static k_timeout_t orientation_wait_timeout(void)
{
    uint64_t now;
    uint64_t remaining;

    if (!orientation_periodic_enabled()) {
        return K_FOREVER;
    }

    now = runtime_uptime_ms();
    if (s_next_orientation_due_ms == 0U) {
        s_next_orientation_due_ms = now + s_orientation_config.sample_period_ms;
    }
    if (now >= s_next_orientation_due_ms) {
        return K_NO_WAIT;
    }
    remaining = s_next_orientation_due_ms - now;
    return K_MSEC(remaining);
}

static bool orientation_due(uint64_t now)
{
    return orientation_periodic_enabled() &&
           s_next_orientation_due_ms != 0U &&
           now >= s_next_orientation_due_ms;
}

static void orientation_periodic_read(void)
{
    nrf_imu_sample_t sample;
    int result;
    uint64_t completion_uptime_ms;

    memset(&sample, 0, sizeof(sample));
    result = s_runtime.core.sources.read_imu(
        s_runtime.core.sources.imu_context,
        &sample);
    completion_uptime_ms = runtime_uptime_ms();
    orientation_feed_sample(
        &sample,
        result == 0,
        completion_uptime_ms);
}

static void orientation_consume_aggregate_refresh(bool did_refresh)
{
    const bool imu_valid = did_refresh &&
        (s_runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_IMU) != 0U;
    const uint64_t completion_uptime_ms = runtime_uptime_ms();

    orientation_feed_sample(
        &s_runtime.core.snapshot.imu,
        imu_valid,
        completion_uptime_ms);
}

static void publish_refresh(uint32_t request_id)
{
    nrf_upstream_sensor_snapshot_t wire;
    nrf_remote_sensor_prepare_result_t prepare_result;
    nrf_sensor_snapshot_result_t refresh_result = NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
    bool did_refresh = false;
    uint64_t completion_uptime_ms = 0U;

    prepare_result = nrf_remote_sensor_runtime_zephyr_prepare(
        &s_worker_node_state,
        &s_runtime);

    if (prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT ||
        prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY) {
        memset(&wire, 0, sizeof(wire));
        wire.request_id = request_id;
        wire.sequence = s_runtime.core.snapshot.sequence;
        wire.result = prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY
            ? NRF_UPSTREAM_SENSOR_RESULT_NOT_READY
            : NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
        wire.flags = runtime_flags();
    } else if (s_runtime.core.imu_ready || s_runtime.core.env_ready) {
        did_refresh = true;
        refresh_result = nrf_remote_sensor_runtime_zephyr_refresh(
            &s_worker_node_state,
            &s_runtime);
        completion_uptime_ms = runtime_uptime_ms();
        copy_runtime_payload(
            request_id,
            did_refresh,
            refresh_result,
            completion_uptime_ms,
            &wire);
    } else {
        copy_runtime_payload(
            request_id,
            false,
            NRF_SENSOR_SNAPSHOT_ERR_NO_DATA,
            0U,
            &wire);
    }

    orientation_consume_aggregate_refresh(did_refresh);

    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    (void)nrf_sensor_session_complete(&s_session, request_id, &wire);
    k_spin_unlock(&s_session_lock, key);
}

static void handle_activation(const nrf_upstream_assign_t *assign)
{
    nrf_sensor_session_activate_result_t activation;
    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    s_activation_queued = false;
    activation = nrf_sensor_session_activate_remote(&s_session, assign);
    k_spin_unlock(&s_session_lock, key);

    if (activation != NRF_SENSOR_SESSION_ACTIVATE_NEW) {
        return;
    }

    nrf_node_state_init(&s_worker_node_state);
    if (nrf_node_state_mark_ready(&s_worker_node_state) != NRF_NODE_STATE_OK ||
        nrf_node_state_assign_role(
            &s_worker_node_state,
            NRF_NODE_ROLE_REMOTE) != NRF_NODE_STATE_OK) {
        nrf_upstream_sensor_snapshot_t failed;
        memset(&failed, 0, sizeof(failed));
        failed.result = NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
        key = k_spin_lock(&s_session_lock);
        (void)nrf_sensor_session_complete(&s_session, 0U, &failed);
        k_spin_unlock(&s_session_lock, key);
        return;
    }

    orientation_reset();
    publish_refresh(0U);
}

static void sensor_worker(void *arg1, void *arg2, void *arg3)
{
    nrf_sensor_event_t event;
    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (true) {
        const int queue_result = k_msgq_get(
            &s_sensor_queue,
            &event,
            orientation_wait_timeout());

        if (queue_result == 0) {
            if (event.type == NRF_SENSOR_EVENT_ACTIVATE) {
                handle_activation(&event.assign);
            } else if (event.type == NRF_SENSOR_EVENT_REFRESH) {
                publish_refresh(event.request_id);
            }
            continue;
        }

        if (orientation_due(runtime_uptime_ms())) {
            orientation_periodic_read();
        }
    }
}

int nrf_sensor_upstream_runtime_zephyr_configure_orientation(
    const nrf_orientation_config_t *config)
{
    k_spinlock_key_t key;

    if (!nrf_orientation_config_valid(config)) {
        return -EINVAL;
    }

    key = k_spin_lock(&s_orientation_lock);
    if (s_started) {
        k_spin_unlock(&s_orientation_lock, key);
        return -EBUSY;
    }
    s_orientation_config = *config;
    s_orientation_config_set = true;
    k_spin_unlock(&s_orientation_lock, key);
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_start(void)
{
    if (s_started) {
        return -EALREADY;
    }

    if (!s_orientation_config_set) {
        nrf_orientation_default_config(&s_orientation_config);
    }
    if (!nrf_orientation_estimator_init(
            &s_orientation_estimator,
            &s_orientation_config)) {
        return -EINVAL;
    }

    nrf_sensor_session_init(&s_session);
    nrf_remote_sensor_runtime_zephyr_init(&s_runtime);
    nrf_node_state_init(&s_worker_node_state);
    k_msgq_purge(&s_sensor_queue);
    s_next_orientation_due_ms = 0U;
    s_activation_queued = false;

    if (k_thread_create(
            &s_sensor_thread,
            s_sensor_stack,
            K_THREAD_STACK_SIZEOF(s_sensor_stack),
            sensor_worker,
            NULL,
            NULL,
            NULL,
            NRF_SENSOR_WORKER_PRIORITY,
            0U,
            K_NO_WAIT) == NULL) {
        return -ENOMEM;
    }

    s_started = true;
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(
    const nrf_upstream_assign_t *assign)
{
    nrf_sensor_event_t event;
    k_spinlock_key_t key;

    if (!s_started) {
        return -EAGAIN;
    }
    if (assign == NULL ||
        nrf_upstream_protocol_validate_assign(assign) != NRF_UPSTREAM_RESULT_OK ||
        assign->role != NRF_UPSTREAM_ROLE_REMOTE) {
        return -EINVAL;
    }

    key = k_spin_lock(&s_session_lock);
    if (s_activation_queued) {
        k_spin_unlock(&s_session_lock, key);
        return 0;
    }
    if (s_session.remote_active) {
        const bool same =
            s_session.node_id == assign->node_id &&
            s_session.mux_channel == assign->mux_channel;
        k_spin_unlock(&s_session_lock, key);
        return same ? 0 : -EALREADY;
    }
    s_activation_queued = true;
    k_spin_unlock(&s_session_lock, key);

    memset(&event, 0, sizeof(event));
    event.type = NRF_SENSOR_EVENT_ACTIVATE;
    event.assign = *assign;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        key = k_spin_lock(&s_session_lock);
        s_activation_queued = false;
        k_spin_unlock(&s_session_lock, key);
        return -ENOSPC;
    }
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_request_refresh(
    const nrf_upstream_sensor_refresh_t *request)
{
    nrf_sensor_session_request_result_t result;
    nrf_sensor_event_t event;
    k_spinlock_key_t key;

    if (!s_started) {
        return -EAGAIN;
    }
    if (request == NULL || request->request_id == 0U) {
        return -EINVAL;
    }

    key = k_spin_lock(&s_session_lock);
    result = nrf_sensor_session_begin_request(
        &s_session,
        request->request_id);
    k_spin_unlock(&s_session_lock, key);

    if (result == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT) {
        return 0;
    }
    if (result == NRF_SENSOR_SESSION_REQUEST_BUSY) {
        return -EBUSY;
    }
    if (result == NRF_SENSOR_SESSION_REQUEST_NOT_REMOTE) {
        return -EAGAIN;
    }
    if (result != NRF_SENSOR_SESSION_REQUEST_ACCEPTED) {
        return -EINVAL;
    }

    memset(&event, 0, sizeof(event));
    event.type = NRF_SENSOR_EVENT_REFRESH;
    event.request_id = request->request_id;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        key = k_spin_lock(&s_session_lock);
        (void)nrf_sensor_session_mark_enqueue_busy(
            &s_session,
            request->request_id);
        k_spin_unlock(&s_session_lock, key);
        return -ENOSPC;
    }
    return 0;
}

bool nrf_sensor_upstream_runtime_zephyr_get_snapshot(
    nrf_upstream_sensor_snapshot_t *snapshot)
{
    k_spinlock_key_t key;
    if (snapshot == NULL || !s_started) {
        return false;
    }
    key = k_spin_lock(&s_session_lock);
    nrf_sensor_session_get_published(&s_session, snapshot);
    k_spin_unlock(&s_session_lock, key);
    return true;
}

bool nrf_sensor_upstream_runtime_zephyr_get_orientation(
    nrf_orientation_output_t *orientation)
{
    k_spinlock_key_t key;
    if (orientation == NULL || !s_started) {
        return false;
    }
    key = k_spin_lock(&s_orientation_lock);
    nrf_orientation_estimator_get(&s_orientation_estimator, orientation);
    k_spin_unlock(&s_orientation_lock, key);
    return true;
}
