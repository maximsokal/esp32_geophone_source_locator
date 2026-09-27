#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_blue_led.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_session.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_verticality_state.h"
#include "nrf_watchdog_zephyr.h"

static unsigned s_thread_create_calls;
static unsigned s_queue_put_calls;
static unsigned s_prepare_calls;
static unsigned s_refresh_calls;
static unsigned s_imu_read_calls;
static unsigned s_runtime_init_calls;
static bool s_fail_next_queue_put;
/* MAX-244 */
static unsigned s_sensor_worker_checkins;
static k_timeout_t s_max_worker_wait = K_NO_WAIT;
static int64_t s_test_uptime_ms = 1234;
static unsigned s_blue_led_init_calls;
static unsigned s_blue_led_update_calls;
static nrf_verticality_state_t s_blue_led_state = NRF_VERTICALITY_FAULT;
static nrf_blue_led_action_t s_blue_led_action = NRF_BLUE_LED_OFF;

int nrf_blue_led_zephyr_init(void)
{
    ++s_blue_led_init_calls;
    return 0;
}

void nrf_blue_led_zephyr_update(
    nrf_verticality_state_t state,
    uint64_t uptime_ms)
{
    (void)uptime_ms;
    ++s_blue_led_update_calls;
    s_blue_led_state = state;
    s_blue_led_action = nrf_blue_led_policy(state);
}

void nrf_watchdog_zephyr_checkin(nrf_watchdog_participant_t participant)
{
    assert(participant == NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    ++s_sensor_worker_checkins;
}
static bool s_exit_worker_when_empty;
static bool s_include_env;
static bool s_force_stationary;
static bool s_force_moving;
static bool s_override_gravity;
static unsigned s_fake_gravity_axis;
static int s_fake_gravity_sign = 1;
static int32_t s_fake_gyro_bias_mdps;
static bool s_advance_worker_time;
static int64_t s_worker_stop_at_ms;
static bool s_change_pressure;
static uint32_t s_fake_pressure_pa;
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
    /* The worker must never block forever: it would stop checking in and
     * the watchdog would reset a healthy idle node. */
    assert(timeout != K_FOREVER);
    assert(timeout <= K_MSEC(1000));
    if (timeout > s_max_worker_wait) {
        s_max_worker_wait = timeout;
    }
    if (q->count == 0U) {
        if (s_advance_worker_time) {
            if (s_test_uptime_ms >= s_worker_stop_at_ms) {
                longjmp(s_worker_exit, 1);
            }
            if (timeout > 0) {
                const int64_t remaining = s_worker_stop_at_ms - s_test_uptime_ms;
                s_test_uptime_ms += timeout < remaining ? timeout : remaining;
            }
            return -ENOMSG;
        }
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
    return s_test_uptime_ms;
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
    sample->gyro_mdps[0] = s_fake_gyro_bias_mdps;
    if (s_force_moving) {
        sample->gyro_mdps[0] = 120000;
    } else if (!s_force_stationary && s_refresh_calls >= 2U) {
        sample->accel_mg[0] = 300;
        sample->gyro_mdps[0] += 30000;
    }
    if (s_override_gravity) {
        sample->accel_mg[s_fake_gravity_axis] = s_fake_gravity_sign * 1000;
    } else {
        sample->accel_mg[2] = 1000;
    }
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
        runtime->core.snapshot.env.pressure_pa = s_change_pressure
            ? s_fake_pressure_pa
            : (s_refresh_calls >= 3U ? 101085U : 101205U);
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

static void run_worker_until_empty(void)
{
    s_exit_worker_when_empty = true;
    if (setjmp(s_worker_exit) == 0) {
        s_worker_entry(NULL, NULL, NULL);
        assert(false);
    }
    s_exit_worker_when_empty = false;
}

static void process_refresh_request(uint32_t request_id)
{
    nrf_upstream_sensor_refresh_t request = {.request_id = request_id};
    s_test_uptime_ms += 100;
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    run_worker_until_empty();
}

static void process_height_reference_request(
    uint8_t action,
    uint32_t request_id,
    uint16_t generation)
{
    nrf_upstream_height_reference_t request = {
        .action = action,
        .request_id = request_id,
        .generation = generation,
    };
    s_test_uptime_ms += 100;
    assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&request) == 0);
    run_worker_until_empty();
}

static void run_worker_advancing(uint32_t duration_ms)
{
    s_advance_worker_time = true;
    s_exit_worker_when_empty = true;
    s_worker_stop_at_ms = s_test_uptime_ms + duration_ms;
    if (setjmp(s_worker_exit) == 0) {
        s_worker_entry(NULL, NULL, NULL);
        assert(false);
    }
    s_exit_worker_when_empty = false;
    s_advance_worker_time = false;
}

static void process_imu_calibration_request(
    uint8_t action,
    uint32_t request_id,
    uint16_t generation)
{
    const nrf_upstream_imu_calibrate_t request = {
        .action = action,
        .request_id = request_id,
        .generation = generation,
    };
    const unsigned refreshes_before = s_refresh_calls;
    const unsigned reads_before = s_imu_read_calls;
    s_test_uptime_ms += 100;
    assert(nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(&request) == 0);
    assert(s_refresh_calls == refreshes_before && s_imu_read_calls == reads_before);
    run_worker_until_empty();
    if (action == NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE) {
        assert(s_refresh_calls == refreshes_before && s_imu_read_calls == reads_before);
        nrf_upstream_sensor_refresh_t competing = {.request_id = request_id + 1U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&competing) == -EBUSY);
        run_worker_advancing(2000U);
        assert(s_refresh_calls == refreshes_before + 1U);
        assert(s_imu_read_calls > reads_before);
    } else {
        assert(s_refresh_calls == refreshes_before + 1U);
        assert(s_imu_read_calls == reads_before + 1U);
    }
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
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(snapshot.reference_generation == 0U && snapshot.relative_z_mm == 0);

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

    /* One check-in per loop iteration: the ACTIVATE pass and the idle
     * pass that escaped the loop. */
    assert(s_sensor_worker_checkins == 2U);
    assert(s_max_worker_wait > K_NO_WAIT);

    assert(s_prepare_calls == 1U);
    assert(s_refresh_calls == 1U);
    assert(s_imu_read_calls == 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U);
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(snapshot.reference_generation == 0U && snapshot.relative_z_mm == 0);

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
    assert(snapshot.orientation_state == nrf_verticality_state_to_orientation_wire(
               NRF_VERTICALITY_STABILIZING));
    assert(s_blue_led_init_calls == 1U && s_blue_led_update_calls >= 3U);
    assert(s_blue_led_state == NRF_VERTICALITY_STABILIZING);
    assert(s_blue_led_action == NRF_BLUE_LED_BLINK_2HZ);
    assert(snapshot.orientation_blink_class == (uint8_t)orientation.blink_class);
    assert(snapshot.orientation_tilt_sin2_ppm == orientation.tilt_sin2_ppm);

    /* The explicit request is accepted after the activation sample completes. */
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    assert(s_queue_put_calls == 3U);

    /* A later aggregate refresh with a valid ENV sample must carry the
     * nRF-computed altitude alongside the legacy pressure/humidity fields. */
    s_include_env = true;
    s_test_uptime_ms += 100;
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
    assert(snapshot.relative_z_mm == 0);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(snapshot.accel_mg[0] == 100);
    assert(snapshot.gyro_mdps[0] == 10000);
    assert(nrf_sensor_upstream_runtime_zephyr_get_orientation(&orientation));
    assert(orientation.filtered_accel_mg[0] == snapshot.accel_mg[0]);
    assert(orientation.filtered_accel_mg[2] == snapshot.accel_mg[2]);
    assert(snapshot.orientation_state == nrf_verticality_state_to_orientation_wire(
               NRF_VERTICALITY_STABILIZING));
    assert(s_blue_led_state == NRF_VERTICALITY_STABILIZING);
    assert(snapshot.orientation_state ==
           nrf_verticality_state_to_orientation_wire(s_blue_led_state));
    assert(s_blue_led_action == nrf_blue_led_policy(s_blue_led_state));
    assert(snapshot.flags & NRF_UPSTREAM_SENSOR_FLAG_ENV_READY);
    assert(snapshot.flags & NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE);
    assert(snapshot.flags & NRF_UPSTREAM_SENSOR_FLAG_FILTERED);

    /* Subsequent samples use dt/(tau+dt): IMU tau=200ms, altitude tau=1s. */
    request.request_id = 21U;
    s_test_uptime_ms += 100;
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    s_exit_worker_when_empty = true;
    if (setjmp(s_worker_exit) == 0) {
        s_worker_entry(NULL, NULL, NULL);
        assert(false);
    }
    s_exit_worker_when_empty = false;
    assert(s_refresh_calls == 3U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == request.request_id);
    assert(snapshot.pressure_pa == 101085U);
    assert(snapshot.altitude_mm == 10909);
    assert(snapshot.accel_mg[0] == 167);
    assert(snapshot.gyro_mdps[0] == 16667);

    /* Stable worker samples satisfy the verticality dwell before capture. */
    s_force_stationary = true;
    for (uint32_t i = 0U; i < 12U; ++i) {
        process_refresh_request(30U + i);
    }
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.orientation_state == NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);

    {
        nrf_upstream_height_reference_t capture = {
            .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE,
            .request_id = 42U,
            .generation = 42U,
        };
        const unsigned refreshes_before = s_refresh_calls;
        const unsigned imu_reads_before = s_imu_read_calls;
        s_test_uptime_ms += 100;
        assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&capture) == 0);
        assert(s_refresh_calls == refreshes_before && s_imu_read_calls == imu_reads_before);
        run_worker_until_empty();
        assert(s_refresh_calls == refreshes_before + 1U);
        assert(s_imu_read_calls == imu_reads_before + 1U);
    }
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(snapshot.reference_generation == 42U);
    assert(snapshot.relative_z_mm == 0);
    {
        const int32_t captured_altitude_mm = snapshot.altitude_mm;
        s_change_pressure = true;
        s_fake_pressure_pa = 100000U;
        process_refresh_request(43U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
        assert(snapshot.reference_generation == 42U);
        assert(snapshot.altitude_mm != captured_altitude_mm);
        assert(snapshot.relative_z_mm == snapshot.altitude_mm - captured_altitude_mm);
        assert(snapshot.relative_z_mm != 0);
    }

    s_force_stationary = false;
    s_force_moving = true;
    process_height_reference_request(
        NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 44U, 43U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    assert(snapshot.reference_generation == 42U);
    assert(snapshot.relative_z_mm == 0);

    s_force_moving = false;
    s_force_stationary = true;
    s_include_env = false;
    process_height_reference_request(
        NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 45U, 44U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(snapshot.reference_generation == 42U);
    assert(snapshot.relative_z_mm == 0);

    process_height_reference_request(
        NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR, 46U, 45U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(snapshot.reference_generation == 45U);
    assert(snapshot.relative_z_mm == 0);

    s_force_stationary = true;
    s_force_moving = false;
    s_include_env = false;
    s_override_gravity = true;
    s_fake_gravity_axis = 0U;
    s_fake_gravity_sign = 1;
    s_fake_gyro_bias_mdps = 14000;
    process_imu_calibration_request(
        NRF_UPSTREAM_IMU_CALIBRATE_CLEAR, 60U, 60U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED);
    assert(snapshot.imu_cal_generation == 60U);
    assert(snapshot.gyro_bias_mdps[0] == 0 && snapshot.gravity_ref_mg[0] == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_get_orientation(&orientation));
    assert(orientation.reason == NRF_ORIENTATION_REASON_UNCONFIGURED);

    process_imu_calibration_request(
        NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE, 61U, 61U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(snapshot.imu_cal_generation == 61U);
    assert(snapshot.gyro_bias_mdps[0] == 14000);
    assert(snapshot.gravity_ref_mg[0] == 1000);
    assert(snapshot.gravity_ref_mg[1] == 0 && snapshot.gravity_ref_mg[2] == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_get_orientation(&orientation));
    assert(orientation.state == NRF_ORIENTATION_STATE_ACCEPTED);
    assert(orientation.filtered_accel_mg[0] == 1000);
    assert(orientation.gyro_peak_mdps == 0U);
    request.request_id = 62U;
    process_refresh_request(request.request_id);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.orientation_state == NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED);
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);

    /* Same assignment is idempotent; a conflicting active identity is rejected. */
    {
        const unsigned queue_calls_before = s_queue_put_calls;
        assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
        assert(s_queue_put_calls == queue_calls_before);
        conflicting.node_id = 3U;
        conflicting.mux_channel = 2U;
        assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&conflicting) == -EALREADY);
        assert(s_queue_put_calls == queue_calls_before);
    }
    return 0;
}
