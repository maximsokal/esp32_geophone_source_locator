#include "nrf_sensor_upstream_runtime_zephyr.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS && \
    defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <zephyr/sys/printk.h>
#endif

#include "nrf_bme280.h"
#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"
#include "nrf_blue_led.h"
#include "nrf_ema_filter.h"
#include "nrf_baro_altitude.h"
#include "nrf_height_reference.h"
#include "nrf_imu_calibration.h"
#include "nrf_logger_led.h"
#include "nrf_logger_led_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_verticality_state.h"
#include "nrf_node_state.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_session.h"
#include "nrf_sensor_bus_zephyr.h"
#include "nrf_timing.h"
#include "nrf_watchdog_zephyr.h"
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
#include "nrf_stage_signal.h"
#ifndef CONFIG_APP_NRF_UPSTREAM_LOSS_TIMEOUT_MS
#define CONFIG_APP_NRF_UPSTREAM_LOSS_TIMEOUT_MS 15000
#endif
#endif

#define NRF_SENSOR_EVENT_QUEUE_DEPTH 4U
#define NRF_SENSOR_WORKER_STACK_SIZE 3072U
#define NRF_SENSOR_WORKER_PRIORITY K_PRIO_PREEMPT(10)
/* MAX-244: longest idle wait, well below NRF_WATCHDOG_MAX_SILENCE_MS. */
#define NRF_SENSOR_WORKER_HEARTBEAT_MS 1000U
#define NRF_SENSOR_ACCEL_RANGE_MG 2000.0
#define NRF_SENSOR_GYRO_RANGE_MDPS 250000.0
#define NRF_SENSOR_ALTITUDE_MIN_MM (-500000.0)
#define NRF_SENSOR_ALTITUDE_MAX_MM 6000000.0
#define NRF_SENSOR_FILTER_TAU_MS 200U
#define NRF_ALTITUDE_FILTER_TAU_MS 1000U

typedef enum {
    NRF_SENSOR_EVENT_ACTIVATE = 1,
    NRF_SENSOR_EVENT_REFRESH = 2,
    NRF_SENSOR_EVENT_HEIGHT_REFERENCE = 3,
    NRF_SENSOR_EVENT_IMU_CALIBRATE = 4,
    NRF_SENSOR_EVENT_SCAN = 5,
} nrf_sensor_event_type_t;

typedef struct {
    uint8_t type;
    uint32_t request_id;
    nrf_upstream_assign_t assign;
    nrf_upstream_height_reference_t height_reference;
    nrf_upstream_imu_calibrate_t imu_calibration;
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
static struct {
    bool active;
    bool has_height_request;
    uint32_t request_id;
    uint64_t next_poll_ms;
    nrf_remote_sensor_prepare_result_t prepare_result;
    nrf_upstream_height_reference_t height_request;
} s_pending_refresh;
static nrf_logger_led_t s_logger_led;
static nrf_node_state_t s_worker_node_state;
static nrf_orientation_config_t s_orientation_config;
static nrf_orientation_estimator_t s_orientation_estimator;
static nrf_verticality_config_t s_verticality_config;
static nrf_verticality_estimator_t s_verticality_estimator;
static nrf_height_reference_t s_height_reference;
static nrf_imu_calibration_t s_imu_calibration;
static struct {
    nrf_ema_filter_t accel[3];
    nrf_ema_filter_t gyro[3];
    nrf_ema_filter_t gyro_motion;
} s_imu_filters;
static nrf_ema_filter_t s_altitude_filter;
static double s_filtered_altitude_mm;
static bool s_altitude_filter_initialized;
static uint64_t s_next_orientation_due_ms;
static bool s_orientation_config_set;
static bool s_started;
static bool s_scan_pending;
static uint32_t s_scan_pending_request_id;
static uint16_t s_scan_address;
static uint32_t s_scan_bitmap[4];
static int s_scan_found;
static bool s_scan_queued;
static uint32_t s_scan_last_request_id;
static bool s_activation_queued;
static bool s_activation_waiting_for_sensors;
static volatile nrf_sensor_upstream_runtime_zephyr_diagnostics_t s_published_diagnostics;
static volatile bool s_diagnostics_published;
static uint32_t s_activation_generation;
static uint32_t s_activation_complete_generation;
static uint32_t s_imu_calibration_request_id;
static uint32_t s_imu_calibration_completed_request_id;
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
static uint32_t s_upstream_transaction_count;
#endif
static uint32_t s_upstream_role_assignment_sequence;
static uint8_t s_upstream_role_assignment_role;
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
static nrf_stage_signal_t s_stage_signal;
static uint32_t s_stage_last_role_assignment_sequence;
static uint32_t s_stage_last_activation_complete_generation;
#endif

static void log_sensor_fault_edges(
    uint32_t previous_fault_mask,
    uint32_t fault_mask,
    uint32_t sequence)
{
    const uint32_t imu_bit = NRF_SENSOR_VALID_IMU;
    const uint32_t env_bit = NRF_SENSOR_VALID_ENV;

    if (((previous_fault_mask ^ fault_mask) & imu_bit) != 0U) {
        nrf_log_runtime_zephyr_append_event(
            (fault_mask & imu_bit) != 0U
                ? NRF_LOG_EVENT_IMU_FAULT_SET
                : NRF_LOG_EVENT_IMU_FAULT_CLEARED,
            imu_bit,
            sequence);
    }
    if (((previous_fault_mask ^ fault_mask) & env_bit) != 0U) {
        nrf_log_runtime_zephyr_append_event(
            (fault_mask & env_bit) != 0U
                ? NRF_LOG_EVENT_ENV_FAULT_SET
                : NRF_LOG_EVENT_ENV_FAULT_CLEARED,
            env_bit,
            sequence);
    }
}

static void publish_refresh(
    uint32_t request_id,
    const nrf_upstream_height_reference_t *height_reference_request);
static void copy_imu_calibration_to_snapshot(nrf_upstream_sensor_snapshot_t *wire);

static void imu_filters_reset(void)
{
    for (size_t axis = 0U; axis < 3U; ++axis) {
        nrf_ema_filter_reset(&s_imu_filters.accel[axis]);
        nrf_ema_filter_reset(&s_imu_filters.gyro[axis]);
    }
    nrf_ema_filter_reset(&s_imu_filters.gyro_motion);
}

static void altitude_filter_reset(void)
{
    nrf_ema_filter_reset(&s_altitude_filter);
    s_filtered_altitude_mm = 0.0;
    s_altitude_filter_initialized = false;
}

static int32_t rounded_i32(double value)
{
    return (int32_t)(value >= 0.0 ? value + 0.5 : -(-value + 0.5));
}

static uint32_t gyro_peak_mdps(const nrf_imu_sample_t *sample)
{
    uint32_t peak = 0U;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        const int64_t value = sample->gyro_mdps[axis];
        const uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

static bool filter_imu_sample(
    const nrf_imu_sample_t *sample,
    uint64_t timestamp_ms,
    nrf_imu_sample_t *filtered)
{
    nrf_imu_sample_t output = *sample;
    double value;

    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (sample->accel_mg[axis] < -NRF_SENSOR_ACCEL_RANGE_MG ||
            sample->accel_mg[axis] > NRF_SENSOR_ACCEL_RANGE_MG ||
            sample->gyro_mdps[axis] < -NRF_SENSOR_GYRO_RANGE_MDPS ||
            sample->gyro_mdps[axis] > NRF_SENSOR_GYRO_RANGE_MDPS) {
            imu_filters_reset();
            return false;
        }
    }
    if (s_imu_filters.accel[0].initialized &&
        timestamp_ms <= s_imu_filters.accel[0].timestamp_ms) {
        for (size_t axis = 0U; axis < 3U; ++axis) {
            output.accel_mg[axis] = rounded_i32(s_imu_filters.accel[axis].value);
            output.gyro_mdps[axis] = rounded_i32(s_imu_filters.gyro[axis].value);
        }
        *filtered = output;
        return true;
    }

    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (nrf_ema_filter_update(
                &s_imu_filters.accel[axis],
                (double)sample->accel_mg[axis],
                timestamp_ms,
                &value) != NRF_EMA_FILTER_UPDATED) {
            imu_filters_reset();
            return false;
        }
        output.accel_mg[axis] = rounded_i32(value);
        if (nrf_ema_filter_update(
                &s_imu_filters.gyro[axis],
                (double)sample->gyro_mdps[axis],
                timestamp_ms,
                &value) != NRF_EMA_FILTER_UPDATED) {
            imu_filters_reset();
            return false;
        }
        output.gyro_mdps[axis] = rounded_i32(value);
    }
    {
        int32_t corrected_gyro[3];
        nrf_imu_sample_t motion_sample = *sample;
        nrf_imu_calibration_correct_gyro_mdps(
            &s_imu_calibration,
            sample->gyro_mdps,
            corrected_gyro);
        memcpy(motion_sample.gyro_mdps, corrected_gyro, sizeof(corrected_gyro));
        if (nrf_ema_filter_update(
                &s_imu_filters.gyro_motion,
                (double)gyro_peak_mdps(&motion_sample),
                timestamp_ms,
                &value) != NRF_EMA_FILTER_UPDATED) {
            imu_filters_reset();
            return false;
        }
    }
    *filtered = output;
    return true;
}

static bool filter_altitude_sample(int32_t altitude_mm, uint64_t timestamp_ms)
{
    double filtered_value;
    const nrf_ema_filter_result_t result = nrf_ema_filter_update(
        &s_altitude_filter,
        (double)altitude_mm,
        timestamp_ms,
        &filtered_value);
    if (result == NRF_EMA_FILTER_INVALID_SAMPLE) {
        altitude_filter_reset();
        return false;
    }
    if (result == NRF_EMA_FILTER_UPDATED) {
        s_filtered_altitude_mm = filtered_value;
        s_altitude_filter_initialized = true;
    }
    return s_altitude_filter_initialized;
}

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
        wire->altitude_mm = rounded_i32(s_filtered_altitude_mm);
    }
    if (wire->valid_mask != 0U) {
        wire->flags |= NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
        wire->flags |= NRF_UPSTREAM_SENSOR_FLAG_FILTERED;
    }

    /*
     * Caller must feed this refresh's IMU sample into the orientation
     * estimator (orientation_consume_aggregate_refresh) before calling this
     * function, so the state read here is fresh, not one cycle stale.
     */
    {
        nrf_orientation_output_t orientation;
        k_spinlock_key_t key = k_spin_lock(&s_orientation_lock);
        nrf_orientation_estimator_get(&s_orientation_estimator, &orientation);
        k_spin_unlock(&s_orientation_lock, key);
        wire->orientation_state = nrf_verticality_state_to_orientation_wire(
            s_verticality_estimator.state);
        wire->orientation_blink_class = (uint8_t)orientation.blink_class;
        wire->orientation_tilt_sin2_ppm = orientation.tilt_sin2_ppm;
    }
    wire->reference_state = (uint8_t)s_height_reference.state;
    wire->reference_generation = s_height_reference.generation;
    wire->relative_z_mm = nrf_height_reference_relative_z_mm(
        &s_height_reference,
        (wire->valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U,
        wire->altitude_mm);
    copy_imu_calibration_to_snapshot(wire);
}

static void orientation_advance_deadline(uint64_t sample_uptime_ms)
{
    const uint64_t period = s_orientation_config.sample_period_ms;
    if (period == 0U) {
        s_next_orientation_due_ms = 0U;
        return;
    }
    s_next_orientation_due_ms = sample_uptime_ms > UINT64_MAX - period
        ? UINT64_MAX : sample_uptime_ms + period;
}

static void orientation_reset(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_orientation_lock);
    (void)nrf_orientation_estimator_init(
        &s_orientation_estimator,
        &s_orientation_config);
    k_spin_unlock(&s_orientation_lock, key);
    s_next_orientation_due_ms = 0U;
    imu_filters_reset();
    altitude_filter_reset();
    nrf_verticality_state_reset(&s_verticality_estimator);
    nrf_blue_led_zephyr_update(
        NRF_VERTICALITY_FAULT,
        runtime_uptime_ms());
}

static void set_orientation_reference_axis(nrf_orientation_reference_axis_t axis)
{
    k_spinlock_key_t key;
    s_orientation_config.reference_axis = axis;
    key = k_spin_lock(&s_orientation_lock);
    (void)nrf_orientation_estimator_init(
        &s_orientation_estimator,
        &s_orientation_config);
    k_spin_unlock(&s_orientation_lock, key);
    nrf_ema_filter_reset(&s_imu_filters.gyro_motion);
    s_next_orientation_due_ms = 0U;
    nrf_verticality_state_reset(&s_verticality_estimator);
}

static void invalidate_sensor_references(bool imu_lost, bool env_lost)
{
    if (imu_lost) {
        const uint16_t generation = s_imu_calibration.generation;
        nrf_imu_calibration_clear(&s_imu_calibration, generation);
        s_imu_calibration.state = NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID;
        set_orientation_reference_axis(NRF_ORIENTATION_AXIS_UNCONFIGURED);
        imu_filters_reset();
    }
    if ((imu_lost || env_lost) && s_height_reference.state == NRF_UPSTREAM_REFERENCE_VALID) {
        s_height_reference.state = NRF_UPSTREAM_REFERENCE_SENSOR_INVALID;
        s_height_reference.altitude_mm = 0;
    }
    if (env_lost) altitude_filter_reset();
}

static void update_gyro_motion(
    const nrf_imu_sample_t *sample,
    bool sample_valid,
    uint64_t sample_uptime_ms)
{
    double filtered_motion;
    if (!sample_valid) {
        nrf_ema_filter_reset(&s_imu_filters.gyro_motion);
        return;
    }
    if (s_imu_filters.gyro_motion.initialized &&
        sample_uptime_ms <= s_imu_filters.gyro_motion.timestamp_ms) {
        return;
    }
    if (nrf_ema_filter_update(
            &s_imu_filters.gyro_motion,
            (double)gyro_peak_mdps(sample),
            sample_uptime_ms,
            &filtered_motion) != NRF_EMA_FILTER_UPDATED) {
        nrf_ema_filter_reset(&s_imu_filters.gyro_motion);
    }
}

static void copy_imu_calibration_to_snapshot(nrf_upstream_sensor_snapshot_t *wire)
{
    memcpy(wire->gyro_bias_mdps, s_imu_calibration.gyro_bias_mdps,
           sizeof(wire->gyro_bias_mdps));
    memcpy(wire->gravity_ref_mg, s_imu_calibration.gravity_ref_mg,
           sizeof(wire->gravity_ref_mg));
    wire->imu_cal_state = (uint8_t)s_imu_calibration.state;
    wire->imu_cal_generation = s_imu_calibration.generation;
}

static void orientation_feed_sample(
    const nrf_imu_sample_t *sample,
    bool sample_valid,
    uint64_t sample_uptime_ms)
{
    nrf_imu_sample_t empty = {0};
    nrf_imu_sample_t corrected;
    int32_t corrected_gyro[3];
    nrf_orientation_output_t orientation;
    k_spinlock_key_t key;
    bool calibration_completed = false;

    if (sample == NULL) {
        sample = &empty;
    }
    corrected = *sample;
    nrf_imu_calibration_correct_gyro_mdps(
        &s_imu_calibration,
        sample->gyro_mdps,
        corrected_gyro);
    memcpy(corrected.gyro_mdps, corrected_gyro, sizeof(corrected.gyro_mdps));
    update_gyro_motion(&corrected, sample_valid, sample_uptime_ms);

    key = k_spin_lock(&s_orientation_lock);
    (void)nrf_orientation_estimator_update(
        &s_orientation_estimator,
        &corrected,
        sample_valid);
    nrf_orientation_estimator_get(&s_orientation_estimator, &orientation);
    k_spin_unlock(&s_orientation_lock, key);

    if (s_imu_calibration.capturing) {
        const uint8_t previous_calibration_state = (uint8_t)s_imu_calibration.state;
        const uint32_t calibration_request_id = s_imu_calibration_request_id;
        const uint32_t corrected_peak = sample_valid ? gyro_peak_mdps(&corrected) : 0U;
        calibration_completed = nrf_imu_calibration_update(
            &s_imu_calibration,
            sample,
            sample_valid,
            orientation.state == NRF_ORIENTATION_STATE_MOVING ||
                corrected_peak >= s_verticality_config.gyro_motion_threshold_mdps,
            sample_uptime_ms);
        if (calibration_completed &&
            previous_calibration_state != (uint8_t)s_imu_calibration.state &&
            (s_imu_calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING ||
             s_imu_calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY)) {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_IMU_CAL_REJECTED,
                calibration_request_id,
                (uint32_t)s_imu_calibration.state);
        }
        if (calibration_completed &&
            s_imu_calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED) {
            set_orientation_reference_axis(s_imu_calibration.reference_axis);
            nrf_imu_calibration_correct_gyro_mdps(
                &s_imu_calibration,
                sample->gyro_mdps,
                corrected_gyro);
            memcpy(corrected.gyro_mdps, corrected_gyro, sizeof(corrected.gyro_mdps));
            update_gyro_motion(&corrected, sample_valid, sample_uptime_ms);
            key = k_spin_lock(&s_orientation_lock);
            (void)nrf_orientation_estimator_update(
                &s_orientation_estimator,
                &corrected,
                sample_valid);
            nrf_orientation_estimator_get(&s_orientation_estimator, &orientation);
            k_spin_unlock(&s_orientation_lock, key);
        }
    }
    (void)nrf_verticality_state_update(
        &s_verticality_estimator,
        sample_valid,
        orientation.sample_valid,
        orientation.tilt_sin2_ppm,
        s_imu_filters.gyro_motion.initialized
            ? (uint32_t)rounded_i32(s_imu_filters.gyro_motion.value)
            : 0U,
        sample_uptime_ms);
    nrf_blue_led_zephyr_update(
        s_verticality_estimator.state,
        sample_uptime_ms);
    orientation_advance_deadline(sample_uptime_ms);
    if (calibration_completed) {
        s_imu_calibration_completed_request_id = s_imu_calibration_request_id;
        s_imu_calibration_request_id = 0U;
    }
}

static bool orientation_periodic_enabled(void)
{
    return (s_orientation_config.reference_axis != NRF_ORIENTATION_AXIS_UNCONFIGURED ||
            s_imu_calibration.capturing) &&
           s_worker_node_state.lifecycle == NRF_NODE_LIFECYCLE_READY &&
           s_worker_node_state.role == NRF_NODE_ROLE_REMOTE &&
           s_runtime.core.imu_ready &&
           s_runtime.core.sources.read_imu != NULL;
}

/*
 * Logger source mapping:
 * | Source | Persistent level from runtime truth |
 * | Node | current lifecycle/role; policy gates display to READY REMOTE |
 * | BMI160 | failed init or ready sensor with latest IMU fault -> ERROR |
 * | BME280 | failed init or ready sensor with latest ENV fault -> ERROR; required on REMOTE |
 * | Log ring | overwritten/lost total nonzero since boot -> WARNING |
 * | TWIS | protocol error total nonzero since boot -> WARNING |
 * | CS/ranging | failure total nonzero since boot -> ERROR |
 * | Watchdog | stale participant total nonzero since boot -> FATAL |
 * | Init | watchdog arm failure latched at boot -> FATAL |
 * | Role | role-lost total nonzero since boot -> WARNING |
 * Pending sensor preparation is not a fault; GREEN means no latched fault.
 */
static void update_logger_led(uint32_t uptime_ms)
{
    nrf_logger_led_state_t state = {0};
    nrf_logger_led_input_t input;
    nrf_log_runtime_zephyr_led_counters_t counters = {0};
    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    const uint32_t role_assignment_sequence = s_upstream_role_assignment_sequence;
    const uint8_t role_assignment_role = s_upstream_role_assignment_role;
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
    const uint32_t transaction_count = s_upstream_transaction_count;
#endif
    k_spin_unlock(&s_session_lock, key);

    (void)nrf_log_runtime_zephyr_get_led_counters(&counters);
    state.node_state = s_worker_node_state;
    if (role_assignment_sequence != 0U &&
        role_assignment_role == NRF_UPSTREAM_ROLE_MAIN) {
        /* Bootstrap commits MAIN without activating REMOTE sensor I/O. */
        state.node_state.role = NRF_NODE_ROLE_MAIN;
        state.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
    }
    state.bmi160_ready = s_runtime.core.imu_ready;
    state.bme280_ready = s_runtime.core.env_ready;
    state.bmi160_init_failed = !s_runtime.core.imu_ready &&
        s_runtime.core.imu_init_result != 0;
    state.bme280_init_failed = !s_runtime.core.env_ready &&
        s_runtime.core.env_init_result != 0;
    state.sensor_fault_mask = s_runtime.core.snapshot.fault_mask;
    state.log_overwritten_total = counters.overwritten_total;
    state.log_lost_internal_total = counters.lost_internal_total;
    state.twis_protocol_error_total = counters.twis_protocol_error_total;
    state.cs_ranging_failure_total = counters.cs_ranging_failure_total;
    state.watchdog_participant_stale_total =
        counters.watchdog_participant_stale_total;
    state.role_lost_total = counters.role_lost_total;
    state.init_boot_fatal = nrf_logger_led_zephyr_init_fatal();

    if (nrf_logger_led_inputs_from_state(&state, &input) == NRF_LOGGER_LED_OK) {
        (void)nrf_logger_led_update(&s_logger_led, &input, uptime_ms);
        {
            /* Console-only before: the ESP32 could not tell which source made the LED yellow/red. */
            nrf_logger_led_severity_t latch_severity;
            nrf_logger_led_source_t latch_source;
            if (nrf_logger_led_take_latch_change(&s_logger_led, &latch_severity, &latch_source)) {
                nrf_log_runtime_zephyr_append_event(
                    NRF_LOG_EVENT_LOGGER_LATCH,
                    (uint32_t)latch_source,
                    (uint32_t)latch_severity);
            }
        }
        nrf_logger_led_output_t apply_output = s_logger_led.output;
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
        nrf_stage_signal_output_t signal_output = {0};
        const bool was_link_lost = nrf_stage_signal_link_lost(&s_stage_signal);

        (void)nrf_stage_signal_set_suppressed(
            &s_stage_signal,
            state.init_boot_fatal);
        (void)nrf_stage_signal_note_transactions(
            &s_stage_signal,
            transaction_count,
            uptime_ms);

        if (role_assignment_sequence != s_stage_last_role_assignment_sequence) {
            s_stage_last_role_assignment_sequence = role_assignment_sequence;
            if (role_assignment_role == NRF_UPSTREAM_ROLE_REMOTE) {
                (void)nrf_stage_signal_post_event(
                    &s_stage_signal,
                    NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE,
                    uptime_ms);
            }
        }

        if (s_activation_complete_generation != 0U &&
            s_activation_complete_generation !=
                s_stage_last_activation_complete_generation) {
            s_stage_last_activation_complete_generation =
                s_activation_complete_generation;
            (void)nrf_stage_signal_post_event(
                &s_stage_signal,
                !s_runtime.core.imu_ready || state.bmi160_init_failed ||
                        state.bme280_init_failed
                    ? NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED
                    : NRF_STAGE_SIGNAL_EVENT_SENSORS_READY,
                uptime_ms);
        }

        (void)nrf_stage_signal_update(
            &s_stage_signal,
            uptime_ms,
            &signal_output);
        {
            nrf_stage_signal_event_t started_event;
            if (nrf_stage_signal_started_event(
                    &s_stage_signal,
                    &started_event) == NRF_STAGE_SIGNAL_OK) {
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
                printk("signal: stage=%s up=%ums\n",
                       nrf_stage_signal_event_name(started_event),
                       (unsigned int)uptime_ms);
#endif
            }
        }
        {
            const bool link_lost = nrf_stage_signal_link_lost(&s_stage_signal);
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
            if (link_lost != was_link_lost) {
                printk("signal: upstream_%s up=%ums\n",
                       link_lost ? "lost" : "restored",
                       (unsigned int)uptime_ms);
            }
#else
            (void)link_lost;
            (void)was_link_lost;
#endif
        }

        if (signal_output.overlay_active) {
            apply_output.green = signal_output.green;
            apply_output.yellow = signal_output.yellow;
            apply_output.red = signal_output.red;
        }
#endif
        if (state.node_state.role == NRF_NODE_ROLE_MAIN) {
            /* MAIN: P1.05 red/P1.07 green; REMOTE aliases have opposite colors. */
            const bool green = apply_output.green;
            apply_output.green = apply_output.red;
            apply_output.red = green;
        }
        (void)nrf_logger_led_zephyr_apply(&apply_output);
    }
}

static void publish_diagnostics(void)
{
    s_published_diagnostics.activation_generation = s_activation_generation;
    s_published_diagnostics.activation_complete_generation =
        s_activation_complete_generation;
    s_published_diagnostics.imu_ready = s_runtime.core.imu_ready;
    s_published_diagnostics.imu_init_result = (int32_t)s_runtime.core.imu_init_result;
    s_published_diagnostics.env_ready = s_runtime.core.env_ready;
    s_published_diagnostics.env_init_result = (int32_t)s_runtime.core.env_init_result;
    s_published_diagnostics.logger_led_output.green = s_logger_led.output.green;
    s_published_diagnostics.logger_led_output.yellow = s_logger_led.output.yellow;
    s_published_diagnostics.logger_led_output.red = s_logger_led.output.red;
    s_published_diagnostics.logger_led_output.severity_latched =
        s_logger_led.output.severity_latched;
    s_published_diagnostics.logger_led_output.reason_source =
        s_logger_led.output.reason_source;
    s_published_diagnostics.logger_policy_invalid_count =
        s_logger_led.invalid_input_count;
    s_published_diagnostics.logger_policy_timestamp_anomaly_count =
        s_logger_led.timestamp_anomaly_count;
    s_published_diagnostics.orientation_state =
        (uint8_t)s_orientation_estimator.output.state;
    s_published_diagnostics.verticality_state =
        (uint8_t)s_verticality_estimator.state;
    s_diagnostics_published = true;
}

/* MAX-244: never wait longer than the worker heartbeat, so an idle
 * (non-REMOTE) worker still checks in with the watchdog. */
static uint64_t sensor_retry_deadline(void)
{
    if (s_worker_node_state.lifecycle != NRF_NODE_LIFECYCLE_READY ||
        s_worker_node_state.role != NRF_NODE_ROLE_REMOTE) return UINT64_MAX;
    uint64_t deadline = UINT64_MAX;
    if (!s_runtime.core.imu_ready) deadline = s_runtime.core.imu_retry_after_ms;
    if (!s_runtime.core.env_ready && s_runtime.core.env_retry_after_ms < deadline)
        deadline = s_runtime.core.env_retry_after_ms;
    return deadline;
}

static k_timeout_t sensor_worker_wait_timeout(void)
{
    const uint64_t now = runtime_uptime_ms();
    uint64_t wait_ms = NRF_SENSOR_WORKER_HEARTBEAT_MS;
    if (s_scan_pending && !s_pending_refresh.active) wait_ms = 1U;
    if (s_pending_refresh.active) {
        const uint64_t remaining = now >= s_pending_refresh.next_poll_ms
            ? 0U : s_pending_refresh.next_poll_ms - now;
        if (remaining < wait_ms) wait_ms = remaining;
    }
    const uint64_t retry = sensor_retry_deadline();
    if (retry != UINT64_MAX) {
        const uint64_t remaining = now >= retry ? 0U : retry - now;
        if (remaining < wait_ms) wait_ms = remaining;
    }

    if (orientation_periodic_enabled()) {
        uint64_t remaining;

        if (s_next_orientation_due_ms == 0U) {
            s_next_orientation_due_ms = now + s_orientation_config.sample_period_ms;
        }
        remaining = now >= s_next_orientation_due_ms
            ? 0U
            : s_next_orientation_due_ms - now;
        if (remaining < wait_ms) {
            wait_ms = remaining;
        }
    }
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
    if (!s_stage_signal.suppressed) {
        if (s_stage_signal.event_active) {
            const uint64_t stage_remaining = nrf_timing_ms_until_u32(
                now,
                s_stage_signal.event_last_ms,
                s_stage_signal.phase_remaining_ms);
            if (stage_remaining < wait_ms) {
                wait_ms = stage_remaining;
            }
        } else if (s_stage_signal.queue_count != 0U) {
            wait_ms = 0U;
        }
    }
#endif
    return K_MSEC(wait_ms);
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
    nrf_imu_sample_t filtered_sample;
    int result;
    uint64_t completion_uptime_ms;

    memset(&sample, 0, sizeof(sample));
    const bool imu_was_ready = s_runtime.core.imu_ready;
    result = s_runtime.core.sources.read_imu(
        s_runtime.core.sources.imu_context,
        &sample);
    if (result == NRF_SENSOR_READ_PENDING) {
        orientation_advance_deadline(runtime_uptime_ms());
        return;
    }
    nrf_remote_sensor_runtime_zephyr_note_imu_read(&s_runtime, result != 0);
    completion_uptime_ms = runtime_uptime_ms();
    if (result == 0) {
        result = filter_imu_sample(&sample, completion_uptime_ms, &filtered_sample)
            ? 0
            : -1;
    }
    orientation_feed_sample(
        result == 0 ? &filtered_sample : NULL,
        result == 0,
        completion_uptime_ms);
    invalidate_sensor_references(imu_was_ready && !s_runtime.core.imu_ready, false);
    if (s_imu_calibration_completed_request_id != 0U) {
        const uint32_t request_id = s_imu_calibration_completed_request_id;
        s_imu_calibration_completed_request_id = 0U;
        publish_refresh(request_id, NULL);
    }
}

static void orientation_consume_aggregate_refresh(bool did_refresh)
{
    bool imu_valid = did_refresh &&
        (s_runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_IMU) != 0U;
    const uint64_t completion_uptime_ms = runtime_uptime_ms();
    const bool imu_pending = did_refresh &&
        ((s_runtime.core.snapshot.valid_mask | s_runtime.core.snapshot.fault_mask) & NRF_SENSOR_VALID_IMU) == 0U;

    if (imu_valid) {
        nrf_imu_sample_t filtered;
        imu_valid = filter_imu_sample(
            &s_runtime.core.snapshot.imu,
            completion_uptime_ms,
            &filtered);
        if (imu_valid) {
            s_runtime.core.snapshot.imu = filtered;
        }
    }
    if (!imu_valid && !imu_pending) {
        s_runtime.core.snapshot.valid_mask &= ~NRF_SENSOR_VALID_IMU;
        s_runtime.core.snapshot.fault_mask |= NRF_SENSOR_VALID_IMU;
        imu_filters_reset();
    }
    if (did_refresh &&
        (s_runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U) {
        const int32_t altitude_mm = nrf_baro_pressure_to_altitude_mm(
            s_runtime.core.snapshot.env.pressure_pa);
        if (!filter_altitude_sample(altitude_mm, completion_uptime_ms)) {
            s_runtime.core.snapshot.valid_mask &= ~NRF_SENSOR_VALID_ENV;
            s_runtime.core.snapshot.fault_mask |= NRF_SENSOR_VALID_ENV;
        }
    } else {
        altitude_filter_reset();
    }

    if (!imu_pending)
        orientation_feed_sample(
            &s_runtime.core.snapshot.imu,
            imu_valid,
            completion_uptime_ms);
}

static void apply_height_reference_request(
    const nrf_upstream_height_reference_t *request,
    bool have_sensor_sample)
{
    const bool imu_valid = have_sensor_sample &&
        (s_runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_IMU) != 0U;
    const bool env_valid = have_sensor_sample &&
        (s_runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U;
    const uint32_t gyro_motion_mdps = s_imu_filters.gyro_motion.initialized
        ? (uint32_t)rounded_i32(s_imu_filters.gyro_motion.value)
        : 0U;
    const bool stationary =
        s_verticality_estimator.state != NRF_VERTICALITY_STABILIZING &&
        gyro_motion_mdps < s_verticality_config.gyro_motion_threshold_mdps;

    if (request != NULL) {
        const nrf_upstream_reference_state_t previous_state = s_height_reference.state;
        bool applied;

        applied = nrf_height_reference_apply(
            &s_height_reference,
            request->action,
            request->generation,
            imu_valid,
            env_valid,
            stationary,
            rounded_i32(s_filtered_altitude_mm));
        if (applied &&
            previous_state != s_height_reference.state &&
            s_height_reference.state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING) {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_HEIGHT_REF_REJECTED,
                request->request_id,
                (uint32_t)s_height_reference.state);
        }
    }
}

static void handle_imu_calibration_request(
    const nrf_upstream_imu_calibrate_t *request)
{
    const uint64_t now = runtime_uptime_ms();
    if (request->action == NRF_UPSTREAM_IMU_CALIBRATE_CLEAR) {
        nrf_imu_calibration_clear(&s_imu_calibration, request->generation);
        set_orientation_reference_axis(NRF_ORIENTATION_AXIS_UNCONFIGURED);
        publish_refresh(request->request_id, NULL);
        return;
    }

    if (!nrf_imu_calibration_begin_capture(
            &s_imu_calibration,
            request->generation,
            now)) {
        return;
    }
    imu_filters_reset();
    s_imu_calibration_request_id = request->request_id;
    if (!s_runtime.core.imu_ready || s_runtime.core.sources.read_imu == NULL) {
        (void)nrf_imu_calibration_update(
            &s_imu_calibration,
            NULL,
            false,
            false,
            now);
        s_imu_calibration_completed_request_id = request->request_id;
        s_imu_calibration_request_id = 0U;
        publish_refresh(request->request_id, NULL);
        return;
    }
    s_next_orientation_due_ms = now;
}

static void complete_refresh(
    uint32_t request_id,
    const nrf_upstream_height_reference_t *height_reference_request,
    nrf_remote_sensor_prepare_result_t prepare_result)
{
    nrf_upstream_sensor_snapshot_t wire;
    nrf_sensor_snapshot_result_t refresh_result = NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
    bool did_refresh = false;
    uint64_t completion_uptime_ms = 0U;
    uint32_t previous_fault_mask;

    if (prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT ||
        prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY) {
        apply_height_reference_request(height_reference_request, false);
        memset(&wire, 0, sizeof(wire));
        wire.request_id = request_id;
        wire.sequence = s_runtime.core.snapshot.sequence;
        wire.result = prepare_result == NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY
            ? NRF_UPSTREAM_SENSOR_RESULT_NOT_READY
            : NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
        wire.flags = runtime_flags();
        wire.reference_state = (uint8_t)s_height_reference.state;
        wire.reference_generation = s_height_reference.generation;
        copy_imu_calibration_to_snapshot(&wire);
    } else if (s_runtime.core.imu_ready || s_runtime.core.env_ready) {
        did_refresh = true;
        const bool imu_was_ready = s_runtime.core.imu_ready;
        const bool env_was_ready = s_runtime.core.env_ready;
        previous_fault_mask = s_runtime.core.snapshot.fault_mask;
        refresh_result = nrf_remote_sensor_runtime_zephyr_refresh(
            &s_worker_node_state,
            &s_runtime);
        completion_uptime_ms = runtime_uptime_ms();
        const uint32_t unfiltered_valid_mask = s_runtime.core.snapshot.valid_mask;
        /*
         * Feed this refresh's IMU sample into the orientation estimator
         * before building the wire snapshot, so orientation_state/
         * blink_class/tilt_sin2_ppm below reflect this cycle rather than
         * the previous one.
         */
        orientation_consume_aggregate_refresh(did_refresh);
        if (s_runtime.core.snapshot.valid_mask != unfiltered_valid_mask &&
            (refresh_result == NRF_SENSOR_SNAPSHOT_OK ||
             refresh_result == NRF_SENSOR_SNAPSHOT_PARTIAL ||
             refresh_result == NRF_SENSOR_SNAPSHOT_ERR_NO_DATA)) {
            refresh_result = s_runtime.core.snapshot.valid_mask != 0U
                ? NRF_SENSOR_SNAPSHOT_PARTIAL : NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
        }
        invalidate_sensor_references(imu_was_ready && !s_runtime.core.imu_ready,
                                     env_was_ready && !s_runtime.core.env_ready);
        log_sensor_fault_edges(
            previous_fault_mask,
            s_runtime.core.snapshot.fault_mask,
            s_runtime.core.snapshot.sequence);
        apply_height_reference_request(height_reference_request, true);
        copy_runtime_payload(
            request_id,
            did_refresh,
            refresh_result,
            completion_uptime_ms,
            &wire);
    } else {
        orientation_consume_aggregate_refresh(did_refresh);
        apply_height_reference_request(height_reference_request, false);
        copy_runtime_payload(
            request_id,
            false,
            NRF_SENSOR_SNAPSHOT_ERR_NO_DATA,
            0U,
            &wire);
    }

    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    (void)nrf_sensor_session_complete(&s_session, request_id, &wire);
    k_spin_unlock(&s_session_lock, key);
}

/*
 * SENSOR_INIT_FAILED only carries the driver result (-2 bus, -3 chip id). The cause of the failed transfers behind it
 * (timeout, EIO, line levels) goes out as SENSOR_BUS_FAULT right after, so a node without a console still says
 * whether the device was silent or the bus was broken.
 */
static void append_sensor_bus_fault(uint32_t sensor_bit, uint8_t first_address, uint8_t last_address)
{
    uint32_t packed;
    if (!nrf_sensor_bus_zephyr_take_fault_word(first_address, last_address, &packed)) return;
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_SENSOR_BUS_FAULT, sensor_bit, packed);
}

static int s_reported_imu_init_result;
static int s_reported_env_init_result;

static void report_sensor_init_result(
    uint32_t sensor_bit,
    int result,
    int *reported,
    uint8_t first_address,
    uint8_t last_address)
{
    if (result == NRF_REMOTE_SENSOR_INIT_PENDING) return;
    if (result == 0) {
        *reported = 0; /* a later failure is news again */
        return;
    }
    if (result == *reported) return;
    *reported = result;
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_SENSOR_INIT_FAILED, sensor_bit, (uint32_t)result);
    append_sensor_bus_fault(sensor_bit, first_address, last_address);
}

/*
 * Reports a changed init failure once, wherever the init was driven from. Cooperative init finishes in the worker's
 * retry step as often as in publish_refresh, so comparing the result before and after one call misses it.
 */
static void report_sensor_init_failures(void)
{
    report_sensor_init_result(NRF_SENSOR_VALID_IMU, s_runtime.core.imu_init_result, &s_reported_imu_init_result,
                              NRF_BMI160_DEFAULT_ADDRESS, NRF_BMI160_ALTERNATE_ADDRESS);
    report_sensor_init_result(NRF_SENSOR_VALID_ENV, s_runtime.core.env_init_result, &s_reported_env_init_result,
                              NRF_BME280_ADDRESS_PRIMARY, NRF_BME280_ADDRESS_SECONDARY);
}

static void publish_refresh(
    uint32_t request_id,
    const nrf_upstream_height_reference_t *height_reference_request)
{
    if (s_pending_refresh.active) return;
    nrf_remote_sensor_prepare_result_t prepare_result;

    prepare_result = nrf_remote_sensor_runtime_zephyr_prepare_cooperative(
        &s_worker_node_state,
        &s_runtime);
    report_sensor_init_failures();


    if (request_id == 0U && (s_runtime.imu_init_active || s_runtime.env_init_active)) {
        s_activation_waiting_for_sensors = true;
        return;
    }
    if (prepare_result != NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT &&
        prepare_result != NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY &&
        s_runtime.core.env_ready &&
        nrf_remote_sensor_runtime_zephyr_begin_env(&s_worker_node_state, &s_runtime,
                                                   runtime_uptime_ms()) == NRF_BME280_PENDING) {
        s_pending_refresh.active = true;
        s_pending_refresh.request_id = request_id;
        s_pending_refresh.prepare_result = prepare_result;
        s_pending_refresh.has_height_request = height_reference_request != NULL;
        if (height_reference_request != NULL) s_pending_refresh.height_request = *height_reference_request;
        s_pending_refresh.next_poll_ms = s_runtime.bme280.conversion_ready_ms;
        return;
    }
    complete_refresh(request_id, height_reference_request, prepare_result);
}

static void poll_pending_refresh(uint64_t now_ms)
{
    if (!s_pending_refresh.active || now_ms < s_pending_refresh.next_poll_ms) return;
    if (nrf_remote_sensor_runtime_zephyr_poll_env(&s_runtime, now_ms) == NRF_BME280_PENDING) {
        s_pending_refresh.next_poll_ms = now_ms + 2U;
        return;
    }
    const uint32_t request_id = s_pending_refresh.request_id;
    const nrf_upstream_height_reference_t *height = s_pending_refresh.has_height_request
        ? &s_pending_refresh.height_request : NULL;
    s_pending_refresh.active = false;
    complete_refresh(request_id, height, s_pending_refresh.prepare_result);
    if (request_id == 0U) s_activation_complete_generation = s_activation_generation;
}

static bool session_matches_assignment(
    const nrf_sensor_session_t *session,
    const nrf_upstream_assign_t *assign)
{
    return session->remote_active &&
           session->node_id == assign->node_id &&
           session->mux_channel == assign->mux_channel;
}

static void handle_activation(const nrf_upstream_assign_t *assign)
{
    bool initial_request_pending;
    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    initial_request_pending =
        session_matches_assignment(&s_session, assign) &&
        s_session.request_pending &&
        s_session.active_request_id == 0U;
    s_activation_queued = false;
    k_spin_unlock(&s_session_lock, key);

    if (!initial_request_pending) {
        return;
    }

    if (s_activation_generation != UINT32_MAX) {
        ++s_activation_generation;
    }
    publish_diagnostics();

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
        s_activation_complete_generation = s_activation_generation;
        publish_diagnostics();
        return;
    }

    orientation_reset();
    publish_refresh(0U, NULL);
    if (!s_pending_refresh.active && !s_activation_waiting_for_sensors)
        s_activation_complete_generation = s_activation_generation;
    publish_diagnostics();
}

static void sensor_worker(void *arg1, void *arg2, void *arg3)
{
    nrf_sensor_event_t event;
    (void)arg1;
    (void)arg2;
    (void)arg3;

    (void)nrf_blue_led_zephyr_init();
    nrf_blue_led_zephyr_update(
        NRF_VERTICALITY_FAULT,
        runtime_uptime_ms());
    nrf_logger_led_reset(&s_logger_led);
    publish_diagnostics();

    while (true) {
        /* MAX-244: check in every iteration and never block longer than
         * the heartbeat period, so an idle (non-REMOTE) worker stays
         * provably alive for the watchdog. */
        nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
        const int queue_result = k_msgq_get(
            &s_sensor_queue,
            &event,
            sensor_worker_wait_timeout());

        if (queue_result == 0) {
            if (event.type == NRF_SENSOR_EVENT_ACTIVATE) {
                handle_activation(&event.assign);
            } else if (event.type == NRF_SENSOR_EVENT_REFRESH) {
                publish_refresh(event.request_id, NULL);
            } else if (event.type == NRF_SENSOR_EVENT_HEIGHT_REFERENCE) {
                publish_refresh(event.request_id, &event.height_reference);
            } else if (event.type == NRF_SENSOR_EVENT_IMU_CALIBRATE) {
                handle_imu_calibration_request(&event.imu_calibration);
            } else if (event.type == NRF_SENSOR_EVENT_SCAN) {
                s_scan_pending = true;
                s_scan_pending_request_id = event.request_id;
                s_scan_address = 0x08U;
                s_scan_found = 0;
                memset(s_scan_bitmap, 0, sizeof(s_scan_bitmap));
                if (!s_runtime.core.imu_ready) s_runtime.core.imu_retry_after_ms = 0U;
                if (!s_runtime.core.env_ready) s_runtime.core.env_retry_after_ms = 0U;
            }
        }
        poll_pending_refresh(runtime_uptime_ms());
        if ((queue_result != 0 || event.type != NRF_SENSOR_EVENT_IMU_CALIBRATE) &&
            orientation_due(runtime_uptime_ms())) {
            const uint64_t now_ms = runtime_uptime_ms();
            /* Reserve the next 100 Hz IMU sample for an imminent aggregate read. */
            if (s_pending_refresh.active && s_pending_refresh.next_poll_ms > now_ms &&
                s_pending_refresh.next_poll_ms - now_ms <= 10U)
                s_next_orientation_due_ms = s_pending_refresh.next_poll_ms;
            else
                orientation_periodic_read();
        }
        if (s_scan_pending && !s_pending_refresh.active) {
            int scan_result = -EAGAIN;
            if (s_worker_node_state.lifecycle == NRF_NODE_LIFECYCLE_READY &&
                s_worker_node_state.role == NRF_NODE_ROLE_REMOTE)
                scan_result = nrf_sensor_bus_zephyr_scan_address((uint8_t)s_scan_address);
            if (scan_result > 0) {
                s_scan_bitmap[s_scan_address / 32U] |= UINT32_C(1) << (s_scan_address % 32U);
                ++s_scan_found;
            }
            if (scan_result < 0 || ++s_scan_address > 0x77U) {
                s_scan_pending = false;
                for (uint16_t i = 0U; i < 4U; ++i)
                    nrf_log_runtime_zephyr_append_event((uint16_t)(NRF_LOG_EVENT_SENSOR_SCAN_WORD0 + i),
                                                        s_scan_pending_request_id, s_scan_bitmap[i]);
                nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_SENSOR_SCAN_DONE,
                    s_scan_pending_request_id, (uint32_t)(scan_result < 0 ? scan_result : s_scan_found));
                k_spinlock_key_t scan_key = k_spin_lock(&s_session_lock);
                s_scan_queued = false;
                k_spin_unlock(&s_session_lock, scan_key);
            }
        }
        const uint64_t retry = sensor_retry_deadline();
        if (retry != UINT64_MAX && runtime_uptime_ms() >= retry) {
            (void)nrf_remote_sensor_runtime_zephyr_prepare_cooperative(&s_worker_node_state, &s_runtime);
            report_sensor_init_failures();
        }
        if (s_activation_waiting_for_sensors && !s_runtime.imu_init_active && !s_runtime.env_init_active) {
            s_activation_waiting_for_sensors = false;
            publish_refresh(0U, NULL);
            if (!s_pending_refresh.active && !s_activation_waiting_for_sensors)
                s_activation_complete_generation = s_activation_generation;
        }
        update_logger_led((uint32_t)runtime_uptime_ms());
        publish_diagnostics();
    }
}

void nrf_sensor_upstream_runtime_zephyr_note_upstream(
    uint32_t transaction_count,
    uint32_t role_assignment_sequence,
    uint8_t role_assignment_role)
{
    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
    s_upstream_transaction_count = transaction_count;
#else
    (void)transaction_count;
#endif
    s_upstream_role_assignment_role = role_assignment_role;
    s_upstream_role_assignment_sequence = role_assignment_sequence;
    k_spin_unlock(&s_session_lock, key);
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
    s_orientation_config.ema_shift = 0U;
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
    s_orientation_config.ema_shift = 0U;
    nrf_verticality_default_config(&s_verticality_config);
    if (!nrf_orientation_estimator_init(
            &s_orientation_estimator,
            &s_orientation_config) ||
        !nrf_verticality_state_init(
            &s_verticality_estimator,
            &s_verticality_config)) {
        return -EINVAL;
    }

    nrf_sensor_session_init(&s_session);
    nrf_height_reference_init(&s_height_reference);
    nrf_imu_calibration_init(&s_imu_calibration);
    s_imu_calibration_request_id = 0U;
    s_imu_calibration_completed_request_id = 0U;
    nrf_remote_sensor_runtime_zephyr_init(&s_runtime);
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (!nrf_ema_filter_init(
                &s_imu_filters.accel[axis],
                NRF_SENSOR_FILTER_TAU_MS,
                -NRF_SENSOR_ACCEL_RANGE_MG,
                NRF_SENSOR_ACCEL_RANGE_MG) ||
            !nrf_ema_filter_init(
                &s_imu_filters.gyro[axis],
                NRF_SENSOR_FILTER_TAU_MS,
                -NRF_SENSOR_GYRO_RANGE_MDPS,
                NRF_SENSOR_GYRO_RANGE_MDPS)) {
            return -EINVAL;
        }
    }
    if (!nrf_ema_filter_init(
            &s_imu_filters.gyro_motion,
            NRF_SENSOR_FILTER_TAU_MS,
            0.0,
            NRF_SENSOR_GYRO_RANGE_MDPS) ||
        !nrf_ema_filter_init(
            &s_altitude_filter,
            NRF_ALTITUDE_FILTER_TAU_MS,
            NRF_SENSOR_ALTITUDE_MIN_MM,
            NRF_SENSOR_ALTITUDE_MAX_MM)) {
        return -EINVAL;
    }
    nrf_node_state_init(&s_worker_node_state);
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
    s_upstream_transaction_count = 0U;
#endif
    s_upstream_role_assignment_sequence = 0U;
    s_upstream_role_assignment_role = NRF_UPSTREAM_ROLE_UNASSIGNED;
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
    s_stage_last_role_assignment_sequence = 0U;
    s_stage_last_activation_complete_generation = 0U;
    if (nrf_stage_signal_init(
            &s_stage_signal,
            (uint32_t)CONFIG_APP_NRF_UPSTREAM_LOSS_TIMEOUT_MS) !=
        NRF_STAGE_SIGNAL_OK) {
        return -EINVAL;
    }
#endif
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
    nrf_sensor_session_t previous_session;
    nrf_sensor_session_activate_result_t activation;
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
        const bool same = session_matches_assignment(&s_session, assign);
        k_spin_unlock(&s_session_lock, key);
        return same ? 0 : -EALREADY;
    }

    previous_session = s_session;
    activation = nrf_sensor_session_activate_remote(&s_session, assign);
    if (activation != NRF_SENSOR_SESSION_ACTIVATE_NEW) {
        k_spin_unlock(&s_session_lock, key);
        return activation == NRF_SENSOR_SESSION_ACTIVATE_CONFLICT
            ? -EALREADY
            : -EINVAL;
    }
    s_activation_queued = true;
    k_spin_unlock(&s_session_lock, key);

    memset(&event, 0, sizeof(event));
    event.type = NRF_SENSOR_EVENT_ACTIVATE;
    event.assign = *assign;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        key = k_spin_lock(&s_session_lock);
        if (s_activation_queued &&
            session_matches_assignment(&s_session, assign) &&
            s_session.request_pending &&
            s_session.active_request_id == 0U) {
            s_session = previous_session;
        }
        s_activation_queued = false;
        k_spin_unlock(&s_session_lock, key);
        return -ENOSPC;
    }
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_request_scan(
    const nrf_upstream_sensor_refresh_t *request)
{
    nrf_sensor_event_t event = {0};
    if (!s_started) return -EAGAIN;
    if (request == NULL || request->request_id == 0U) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&s_session_lock);
    if (!s_session.remote_active) {
        k_spin_unlock(&s_session_lock, key);
        return -EAGAIN;
    }
    if (s_scan_last_request_id == request->request_id) {
        k_spin_unlock(&s_session_lock, key);
        return 0;
    }
    if (s_scan_queued) {
        k_spin_unlock(&s_session_lock, key);
        return -EBUSY;
    }
    event.type = NRF_SENSOR_EVENT_SCAN;
    event.request_id = request->request_id;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        k_spin_unlock(&s_session_lock, key);
        return -ENOSPC;
    }
    s_scan_last_request_id = request->request_id;
    s_scan_queued = true;
    k_spin_unlock(&s_session_lock, key);
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

int nrf_sensor_upstream_runtime_zephyr_request_height_reference(
    const nrf_upstream_height_reference_t *request)
{
    nrf_sensor_session_request_result_t result;
    nrf_sensor_event_t event;
    k_spinlock_key_t key;

    if (!s_started) {
        return -EAGAIN;
    }
    if (request == NULL || request->request_id == 0U ||
        (request->action != NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE &&
         request->action != NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR)) {
        return -EINVAL;
    }

    key = k_spin_lock(&s_session_lock);
    result = nrf_sensor_session_begin_request(&s_session, request->request_id);
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
    event.type = NRF_SENSOR_EVENT_HEIGHT_REFERENCE;
    event.request_id = request->request_id;
    event.height_reference = *request;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        key = k_spin_lock(&s_session_lock);
        (void)nrf_sensor_session_mark_enqueue_busy(&s_session, request->request_id);
        k_spin_unlock(&s_session_lock, key);
        return -ENOSPC;
    }
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(
    const nrf_upstream_imu_calibrate_t *request)
{
    nrf_sensor_session_request_result_t result;
    nrf_sensor_event_t event;
    k_spinlock_key_t key;

    if (!s_started) {
        return -EAGAIN;
    }
    if (request == NULL || request->request_id == 0U ||
        (request->action != NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE &&
         request->action != NRF_UPSTREAM_IMU_CALIBRATE_CLEAR)) {
        return -EINVAL;
    }

    key = k_spin_lock(&s_session_lock);
    result = nrf_sensor_session_begin_request(&s_session, request->request_id);
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
    event.type = NRF_SENSOR_EVENT_IMU_CALIBRATE;
    event.request_id = request->request_id;
    event.imu_calibration = *request;
    if (k_msgq_put(&s_sensor_queue, &event, K_NO_WAIT) != 0) {
        key = k_spin_lock(&s_session_lock);
        (void)nrf_sensor_session_mark_enqueue_busy(&s_session, request->request_id);
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

bool nrf_sensor_upstream_runtime_zephyr_get_diagnostics(
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL || !s_diagnostics_published) {
        return false;
    }

    diagnostics->activation_generation =
        s_published_diagnostics.activation_generation;
    diagnostics->activation_complete_generation =
        s_published_diagnostics.activation_complete_generation;
    diagnostics->imu_ready = s_published_diagnostics.imu_ready;
    diagnostics->imu_init_result = s_published_diagnostics.imu_init_result;
    diagnostics->imu_state = nrf_bmi160_zephyr_state();
    diagnostics->env_ready = s_published_diagnostics.env_ready;
    diagnostics->env_init_result = s_published_diagnostics.env_init_result;
    diagnostics->env_state = nrf_bme280_zephyr_state();
    diagnostics->logger_led_output.green =
        s_published_diagnostics.logger_led_output.green;
    diagnostics->logger_led_output.yellow =
        s_published_diagnostics.logger_led_output.yellow;
    diagnostics->logger_led_output.red =
        s_published_diagnostics.logger_led_output.red;
    diagnostics->logger_led_output.severity_latched =
        s_published_diagnostics.logger_led_output.severity_latched;
    diagnostics->logger_led_output.reason_source =
        s_published_diagnostics.logger_led_output.reason_source;
    diagnostics->logger_policy_invalid_count =
        s_published_diagnostics.logger_policy_invalid_count;
    diagnostics->logger_policy_timestamp_anomaly_count =
        s_published_diagnostics.logger_policy_timestamp_anomaly_count;
    diagnostics->orientation_state =
        s_published_diagnostics.orientation_state;
    diagnostics->verticality_state =
        s_published_diagnostics.verticality_state;
    return true;
}
