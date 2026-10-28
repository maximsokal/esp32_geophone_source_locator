#include <assert.h>
#include <errno.h>
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS && \
    defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#include <stdarg.h>
#include <zephyr/sys/printk.h>
#endif
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "nrf_blue_led.h"
#include "nrf_logger_led_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_bus_zephyr.h"
#include "nrf_sensor_session.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_verticality_state.h"
#include "nrf_watchdog_zephyr.h"

extern struct k_msgq s_sensor_queue;
#if defined(NRF_REAL_SENSOR_WORKER_TEST)
extern void fake_sensor_bus_setup(void);
extern void fake_sensor_bus_set_bad_pressure(void);
extern void fake_sensor_bus_restore_pressure(void);
extern void fake_sensor_bus_suspend_imu(void);
extern void fake_sensor_bus_reset_env(void);
extern void fake_sensor_bus_hold_env_nvm(bool busy);
extern void fake_sensor_bus_set_imu_present(bool present);
extern void fake_sensor_bus_set_drdy_mask(uint8_t mask);
extern unsigned fake_sensor_bus_imu_id_probes;
extern unsigned fake_sensor_bus_sleep_count;
extern unsigned fake_sensor_bus_imu_reads;
extern uint64_t fake_sensor_bus_imu_max_gap_ms;
extern unsigned fake_sensor_bus_imu_states, fake_sensor_bus_env_states;
extern unsigned fake_sensor_bus_scan_reads;
#endif

/* Each -D...TEST variant uses a different subset of the helpers below. */
#if defined(__GNUC__) || defined(__clang__)
#define NRF_TEST_MAYBE_UNUSED __attribute__((unused))
#else
#define NRF_TEST_MAYBE_UNUSED
#endif

static unsigned s_thread_create_calls;
static int s_sensor_worker_priority;
static unsigned s_queue_put_calls;
static unsigned s_prepare_calls;
static unsigned s_refresh_calls;
static unsigned s_imu_read_calls;
static unsigned s_runtime_init_calls;
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
static unsigned s_empty_worker_returns_remaining;
static bool s_include_env;
static bool s_force_stationary;
static bool s_fail_imu_reads;
static bool s_async_env;
static bool s_force_moving;
static bool s_override_gravity;
static unsigned s_fake_gravity_axis;
static int s_fake_gravity_sign = 1;
static int32_t s_fake_gyro_bias_mdps;
static bool s_advance_worker_time;
static int64_t s_worker_stop_at_ms;
static bool s_change_pressure;
static uint32_t s_fake_pressure_pa;
static uint32_t s_test_fault_mask;
static jmp_buf s_worker_exit;
static void (*s_worker_entry)(void *, void *, void *);
static struct {
    uint16_t event_id;
    uint32_t arg0;
    uint32_t arg1;
} s_log_events[512];
static unsigned s_log_event_count;
static bool s_init_fatal;
static bool s_fail_imu_init;
static bool s_fail_env_init;
static nrf_log_runtime_zephyr_led_counters_t s_led_counters;

#define NRF_TEST_LED_SAMPLE_CAPACITY 2048U
static nrf_logger_led_output_t s_led_samples[NRF_TEST_LED_SAMPLE_CAPACITY];
static uint64_t s_led_sample_times[NRF_TEST_LED_SAMPLE_CAPACITY];
static unsigned s_led_sample_count;
#define NRF_TEST_WORKER_WAIT_CAPACITY 4096U
static k_timeout_t s_worker_waits[NRF_TEST_WORKER_WAIT_CAPACITY];
static unsigned s_worker_wait_count;

#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS && \
    defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
#define NRF_TEST_SIGNAL_LOG_CAPACITY 32U
#define NRF_TEST_SIGNAL_LOG_LENGTH 128U
static char s_signal_logs[NRF_TEST_SIGNAL_LOG_CAPACITY][NRF_TEST_SIGNAL_LOG_LENGTH];
static unsigned s_signal_log_count;

int printk(const char *format, ...)
{
    int result;
    va_list args;

    if (s_signal_log_count >= NRF_TEST_SIGNAL_LOG_CAPACITY) {
        assert(false);
        return -1;
    }
    va_start(args, format);
    result = vsnprintf(
        s_signal_logs[s_signal_log_count],
        NRF_TEST_SIGNAL_LOG_LENGTH,
        format,
        args);
    va_end(args);
    ++s_signal_log_count;
    return result;
}
#endif

void nrf_log_runtime_zephyr_append_event(uint16_t event_id, uint32_t arg0, uint32_t arg1)
{
    assert(s_log_event_count < sizeof(s_log_events) / sizeof(s_log_events[0]));
    s_log_events[s_log_event_count].event_id = event_id;
    s_log_events[s_log_event_count].arg0 = arg0;
    s_log_events[s_log_event_count].arg1 = arg1;
    ++s_log_event_count;
}

nrf_log_error_t nrf_log_runtime_zephyr_get_led_counters(
    nrf_log_runtime_zephyr_led_counters_t *counters)
{
    if (counters == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    *counters = s_led_counters;
    return NRF_LOG_ERROR_OK;
}

nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_apply(
    const nrf_logger_led_output_t *output)
{
    assert(output != NULL);
    assert(s_led_sample_count < NRF_TEST_LED_SAMPLE_CAPACITY);
    s_led_samples[s_led_sample_count] = *output;
    s_led_sample_times[s_led_sample_count] = (uint64_t)s_test_uptime_ms;
    ++s_led_sample_count;
    return NRF_LOGGER_LED_ZEPHYR_OK;
}

bool nrf_logger_led_zephyr_init_fatal(void)
{
    return s_init_fatal;
}

NRF_TEST_MAYBE_UNUSED static unsigned log_event_count(uint16_t event_id)
{
    unsigned count = 0U;
    for (unsigned index = 0U; index < s_log_event_count; ++index) {
        if (s_log_events[index].event_id == event_id) {
            ++count;
        }
    }
    return count;
}

/* Index of the first logged event with this id and arg0, or UINT_MAX. */
NRF_TEST_MAYBE_UNUSED static unsigned find_log_event(uint16_t event_id, uint32_t arg0)
{
    for (unsigned index = 0U; index < s_log_event_count; ++index) {
        if (s_log_events[index].event_id == event_id && s_log_events[index].arg0 == arg0) {
            return index;
        }
    }
    return UINT_MAX;
}

NRF_TEST_MAYBE_UNUSED static void assert_last_log_event(
    uint16_t event_id, uint32_t arg0, uint32_t arg1)
{
    unsigned last = s_log_event_count;
    assert(last != 0U);
    /* A logger-LED latch can be recorded right after the event under test. */
    while (event_id != NRF_LOG_EVENT_LOGGER_LATCH && last > 1U &&
           s_log_events[last - 1U].event_id == NRF_LOG_EVENT_LOGGER_LATCH) {
        --last;
    }
    assert(s_log_events[last - 1U].event_id == event_id);
    assert(s_log_events[last - 1U].arg0 == arg0);
    assert(s_log_events[last - 1U].arg1 == arg1);
}

NRF_TEST_MAYBE_UNUSED static void fill_sensor_queue_and_check_capacity(void)
{
    uint8_t item[64] = {0U};

    assert(s_sensor_queue.msg_size <= sizeof(item));
    assert(s_sensor_queue.max_msgs == 4U);
    assert(s_sensor_queue.count == 0U);
    for (unsigned index = 0U; index + 1U < s_sensor_queue.max_msgs; ++index) {
        assert(k_msgq_put(&s_sensor_queue, item, K_NO_WAIT) == 0);
    }
    assert(s_sensor_queue.count == s_sensor_queue.max_msgs - 1U);
    assert(k_msgq_put(&s_sensor_queue, item, K_NO_WAIT) == 0);
    assert(s_sensor_queue.count == s_sensor_queue.max_msgs);
    assert(k_msgq_put(&s_sensor_queue, item, K_NO_WAIT) == -ENOMSG);
}

void k_msgq_purge(struct k_msgq *q)
{
    q->head = q->tail = q->count = 0U;
}

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout)
{
    (void)timeout;
    ++s_queue_put_calls;
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
    assert(s_worker_wait_count < NRF_TEST_WORKER_WAIT_CAPACITY);
    s_worker_waits[s_worker_wait_count++] = timeout;
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
            if (s_empty_worker_returns_remaining == 0U) {
                longjmp(s_worker_exit, 1);
            }
            --s_empty_worker_returns_remaining;
            return -ENOMSG;
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
    s_sensor_worker_priority = prio;
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
#if defined(NRF_REAL_SENSOR_WORKER_TEST)
void fake_sensor_bus_advance(int32_t ms) { s_test_uptime_ms += ms; }
#endif

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

#if !defined(NRF_REAL_SENSOR_WORKER_TEST)
int nrf_bmi160_zephyr_state(void) { return -1; }
int nrf_bme280_zephyr_state(void) { return -1; }
int nrf_sensor_bus_zephyr_scan_address(uint8_t address) { (void)address; return 0; }

/* The packed cause of the failed transfers behind an init failure: slot 0 is the IMU range, slot 1 the ENV range. */
static struct {
    bool valid;
    uint32_t word;
} s_fake_faults[2];

bool nrf_sensor_bus_zephyr_take_fault_word(uint8_t first_address, uint8_t last_address, uint32_t *packed)
{
    /* The runtime must ask for exactly the addresses each driver can use: BMI160 0x68-0x69, BME280 0x76-0x77. */
    assert((first_address == 0x68U && last_address == 0x69U) || (first_address == 0x76U && last_address == 0x77U));
    const size_t slot = first_address == 0x68U ? 0U : 1U;
    if (packed == NULL || !s_fake_faults[slot].valid) return false;
    *packed = s_fake_faults[slot].word;
    s_fake_faults[slot].valid = false;
    return true;
}
static int fake_read_imu(void *context, nrf_imu_sample_t *sample)
{
    (void)context;
    ++s_imu_read_calls;
    memset(sample, 0, sizeof(*sample));
    if (s_fail_imu_reads) return -EIO;
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

void nrf_remote_sensor_runtime_zephyr_note_imu_read(
    nrf_remote_sensor_runtime_zephyr_t *runtime, bool faulted)
{
    if (!runtime->core.imu_ready) return;
    if (!faulted) runtime->core.imu_read_failures = 0U;
    else if (++runtime->core.imu_read_failures >= NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES) {
        runtime->core.imu_ready = false;
        runtime->bmi160.state = NRF_BMI160_RECOVER;
    }
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    (void)node_state;
    ++s_prepare_calls;
    runtime->core.imu_ready = !s_fail_imu_init;
    runtime->core.imu_init_result = s_fail_imu_init ? -EIO : 0;
    runtime->core.imu_retry_after_ms = s_fail_imu_init
        ? (uint64_t)s_test_uptime_ms + NRF_REMOTE_SENSOR_RETRY_MS : 0U;
    runtime->core.env_ready = s_include_env && !s_fail_env_init;
    runtime->core.env_init_result = s_fail_env_init ? -ENODEV : 0;
    if (!runtime->core.env_ready && runtime->core.env_retry_after_ms <= (uint64_t)s_test_uptime_ms)
        runtime->core.env_retry_after_ms = (uint64_t)s_test_uptime_ms + NRF_REMOTE_SENSOR_RETRY_MS;
    if (runtime->core.env_ready) runtime->core.env_retry_after_ms = 0U;
    runtime->core.sources.read_imu = fake_read_imu;
    runtime->core.sources.imu_context = NULL;
    return NRF_REMOTE_SENSOR_PREPARE_PARTIAL;
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare_cooperative(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    return nrf_remote_sensor_runtime_zephyr_prepare(node_state, runtime);
}

nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_begin_env(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms)
{
    (void)node_state;
    if (!s_async_env) return NRF_BME280_OK;
    runtime->bme280.conversion_pending = true;
    runtime->bme280.conversion_ready_ms = now_ms + 47U;
    return NRF_BME280_PENDING;
}

nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_poll_env(
    nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms)
{
    if (now_ms < runtime->bme280.conversion_ready_ms) return NRF_BME280_PENDING;
    runtime->bme280.conversion_pending = false;
    return NRF_BME280_OK;
}

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    (void)node_state;
    ++s_refresh_calls;
    runtime->core.snapshot.valid_mask = NRF_SENSOR_VALID_IMU & ~s_test_fault_mask;
    runtime->core.snapshot.fault_mask = s_test_fault_mask;
    if (s_include_env) {
        runtime->core.snapshot.valid_mask |= NRF_SENSOR_VALID_ENV & ~s_test_fault_mask;
        runtime->core.snapshot.env.temperature_milli_c = 21500;
        runtime->core.snapshot.env.pressure_pa = s_change_pressure
            ? s_fake_pressure_pa
            : (s_refresh_calls >= 3U ? 101085U : 101205U);
        runtime->core.snapshot.env.humidity_milli_percent = 50300U;
    }
    if (runtime->core.env_ready) {
        if ((s_test_fault_mask & NRF_SENSOR_VALID_ENV) != 0U) {
            if (++runtime->core.env_read_failures >= NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES)
                runtime->core.env_ready = false;
        } else {
            runtime->core.env_read_failures = 0U;
        }
    }
    return fake_read_imu(NULL, &runtime->core.snapshot.imu) == 0
        ? NRF_SENSOR_SNAPSHOT_PARTIAL
        : NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
}

#endif

NRF_TEST_MAYBE_UNUSED static nrf_upstream_assign_t make_assign(void)
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

#if defined(NRF_STAGE_SIGNAL_SUCCESS_TEST) || defined(NRF_STAGE_SIGNAL_FAILED_TEST)
static void run_worker_ticks(unsigned tick_count)
{
    for (unsigned tick = 0U; tick < tick_count; ++tick) {
        s_empty_worker_returns_remaining = 1U;
        run_worker_until_empty();
    }
}
#endif

static void run_worker_advancing(uint32_t duration_ms);

NRF_TEST_MAYBE_UNUSED static void assert_last_applied_matches_policy(void)
{
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};
    const nrf_logger_led_output_t *applied;

    assert(s_led_sample_count != 0U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    applied = &s_led_samples[s_led_sample_count - 1U];
    assert(applied->green == diagnostics.logger_led_output.green);
    assert(applied->yellow == diagnostics.logger_led_output.yellow);
    assert(applied->red == diagnostics.logger_led_output.red);
    assert(applied->severity_latched == diagnostics.logger_led_output.severity_latched);
    assert(applied->reason_source == diagnostics.logger_led_output.reason_source);
}

#if defined(NRF_STAGE_SIGNAL_SUCCESS_TEST) || defined(NRF_STAGE_SIGNAL_FAILED_TEST)
static void assert_last_applied_colors(bool green, bool yellow, bool red)
{
    assert(s_led_sample_count != 0U);
    assert(s_led_samples[s_led_sample_count - 1U].green == green);
    assert(s_led_samples[s_led_sample_count - 1U].yellow == yellow);
    assert(s_led_samples[s_led_sample_count - 1U].red == red);
}

NRF_TEST_MAYBE_UNUSED static bool led_colors_recorded_at(
    uint64_t time_ms,
    bool green,
    bool yellow,
    bool red)
{
    for (unsigned index = 0U; index < s_led_sample_count; ++index) {
        if (s_led_sample_times[index] == time_ms &&
            s_led_samples[index].green == green &&
            s_led_samples[index].yellow == yellow &&
            s_led_samples[index].red == red) {
            return true;
        }
    }
    return false;
}
#endif

#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS && \
    (defined(NRF_STAGE_SIGNAL_SUCCESS_TEST) || \
     defined(NRF_STAGE_SIGNAL_FAILED_TEST))
static unsigned signal_log_count_containing(const char *needle)
{
    unsigned count = 0U;
    for (unsigned index = 0U; index < s_signal_log_count; ++index) {
        if (strstr(s_signal_logs[index], needle) != NULL) {
            ++count;
        }
    }
    return count;
}

static void stage_tick_after(uint32_t delta_ms)
{
    s_test_uptime_ms += (int64_t)delta_ms;
    run_worker_ticks(1U);
}

static void assert_stage_pattern(
    uint8_t flash_count,
    bool green,
    bool yellow,
    bool red)
{
    assert(flash_count != 0U);
    assert_last_applied_colors(false, false, false);

    stage_tick_after(300U);
    assert_last_applied_colors(green, yellow, red);
    for (uint8_t index = 0U; index < flash_count; ++index) {
        stage_tick_after(150U);
        assert_last_applied_colors(false, false, false);
        stage_tick_after(150U);
        if (index + 1U < flash_count) {
            assert_last_applied_colors(green, yellow, red);
        } else {
            assert_last_applied_colors(false, false, false);
        }
    }
    stage_tick_after(300U);
}

#if defined(NRF_STAGE_SIGNAL_SUCCESS_TEST)
static void test_first_transaction_signal(void)
{
    unsigned wait_start;
    uint64_t signal_start_ms;

    assert_last_applied_colors(false, false, false);
    assert_last_applied_matches_policy();

    s_test_uptime_ms = (int64_t)(UINT32_MAX - 100U);
    signal_start_ms = (uint64_t)s_test_uptime_ms;
    wait_start = s_worker_wait_count;
    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        1U,
        0U,
        NRF_UPSTREAM_ROLE_UNASSIGNED);
    run_worker_ticks(1U);
    assert_last_applied_colors(false, false, false);
    assert(signal_log_count_containing("signal: stage=I2C_ALIVE ") == 1U);
    assert(s_worker_wait_count > wait_start + 1U);
    assert(s_worker_waits[wait_start + 1U] == K_MSEC(300U));

    wait_start = s_worker_wait_count;
    run_worker_advancing(900U);
    assert(s_test_uptime_ms == (int64_t)(signal_start_ms + 900U));
    assert(s_worker_wait_count >= wait_start + 4U);
    assert(s_worker_waits[wait_start] == K_MSEC(300U));
    assert(s_worker_waits[wait_start + 1U] == K_MSEC(150U));
    assert(s_worker_waits[wait_start + 2U] == K_MSEC(150U));
    assert(s_worker_waits[wait_start + 3U] == K_MSEC(300U));
    assert(led_colors_recorded_at(signal_start_ms + 300U, true, false, false));
    assert(led_colors_recorded_at(signal_start_ms + 450U, false, false, false));
    assert_last_applied_colors(false, false, false);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: stage=I2C_ALIVE ") == 1U);
}

static void test_assignment_and_sensor_ready_signals(void)
{
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};

    assert_last_applied_colors(false, false, false);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.logger_led_output.green);
    assert(signal_log_count_containing("signal: stage=ROLE_REMOTE ") == 1U);
    assert(signal_log_count_containing("signal: stage=SENSORS_READY ") == 0U);

    assert_stage_pattern(2U, true, false, false);
    assert(signal_log_count_containing("signal: stage=ROLE_REMOTE ") == 1U);
    assert(signal_log_count_containing("signal: stage=SENSORS_READY ") == 1U);

    assert_stage_pattern(3U, true, false, false);
    assert_last_applied_colors(true, false, false);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: stage=SENSORS_READY ") == 1U);

    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        1U,
        1U,
        NRF_UPSTREAM_ROLE_REMOTE);
    run_worker_ticks(1U);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: stage=ROLE_REMOTE ") == 1U);
    assert(signal_log_count_containing("signal: stage=SENSORS_READY ") == 1U);
}

static void test_upstream_loss_restore_and_init_fatal(void)
{
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};
    const unsigned lost_before_rebase =
        signal_log_count_containing("signal: upstream_lost ");
    unsigned restored_after_rebase;

    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        2U,
        1U,
        NRF_UPSTREAM_ROLE_REMOTE);
    run_worker_ticks(1U);
    restored_after_rebase =
        signal_log_count_containing("signal: upstream_restored ");

    run_worker_advancing(
        (uint32_t)CONFIG_APP_NRF_UPSTREAM_LOSS_TIMEOUT_MS);
    assert(signal_log_count_containing("signal: upstream_lost ") ==
           lost_before_rebase + 1U);
    assert_last_applied_colors(false, true, false);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.logger_led_output.green);

    stage_tick_after(150U);
    assert_last_applied_colors(true, false, false);
    stage_tick_after(1850U);
    assert_last_applied_colors(false, true, false);
    assert(signal_log_count_containing("signal: upstream_lost ") ==
           lost_before_rebase + 1U);

    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        3U,
        1U,
        NRF_UPSTREAM_ROLE_REMOTE);
    run_worker_ticks(1U);
    assert_last_applied_colors(true, false, false);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: upstream_restored ") ==
           restored_after_rebase + 1U);
    run_worker_ticks(1U);
    assert(signal_log_count_containing("signal: upstream_restored ") ==
           restored_after_rebase + 1U);

    s_init_fatal = true;
    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        3U,
        2U,
        NRF_UPSTREAM_ROLE_REMOTE);
    run_worker_ticks(1U);
    assert_last_applied_colors(false, false, true);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: stage=ROLE_REMOTE ") == 1U);
    assert(signal_log_count_containing("signal: upstream_lost ") ==
           lost_before_rebase + 1U);
    assert(signal_log_count_containing("signal: upstream_restored ") ==
           restored_after_rebase + 1U);
}

static void test_successful_stage_signals(void)
{
    nrf_orientation_config_t orientation_config;
    nrf_upstream_assign_t assign = make_assign();
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};

    nrf_orientation_default_config(&orientation_config);
    assert(nrf_sensor_upstream_runtime_zephyr_configure_orientation(
               &orientation_config) == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    assert(s_sensor_worker_priority == 10);
    run_worker_ticks(1U);
    assert_last_applied_colors(false, false, false);
    assert_last_applied_matches_policy();
    test_first_transaction_signal();

    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        1U,
        1U,
        NRF_UPSTREAM_ROLE_REMOTE);
    run_worker_until_empty();
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.activation_complete_generation == 1U);
    assert(diagnostics.imu_ready);
    assert(diagnostics.logger_led_output.green);

    test_assignment_and_sensor_ready_signals();
    test_upstream_loss_restore_and_init_fatal();
}
#endif

#if defined(NRF_STAGE_SIGNAL_FAILED_TEST)
static void test_failed_sensor_signal(void)
{
    nrf_orientation_config_t orientation_config;
    nrf_upstream_assign_t assign = make_assign();
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};
    nrf_upstream_sensor_snapshot_t snapshot = {0};

    nrf_orientation_default_config(&orientation_config);
    assert(nrf_sensor_upstream_runtime_zephyr_configure_orientation(
               &orientation_config) == 0);
    s_fail_imu_init = true;
    s_fail_env_init = true;
    s_include_env = true;
    /*
     * The packed causes behind each init failure (layout in nrf_sensor_bus.h): a timeout at 0x68 with SDA held low,
     * four failures; a plain EIO at 0x77 with both lines high, two failures.
     */
    const uint32_t imu_cause = UINT32_C(0x68) | (UINT32_C(1) << 8) | (UINT32_C(116) << 16) |
                               (UINT32_C(1) << 25) | (UINT32_C(1) << 26) | (UINT32_C(4) << 27);
    const uint32_t env_cause = UINT32_C(0x77) | (UINT32_C(2) << 8) | (UINT32_C(5) << 16) |
                               (UINT32_C(3) << 24) | (UINT32_C(1) << 26) | (UINT32_C(2) << 27);
    s_fake_faults[0].valid = true;
    s_fake_faults[0].word = imu_cause;
    s_fake_faults[1].valid = true;
    s_fake_faults[1].word = env_cause;
    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    assert(s_sensor_worker_priority == 10);
    run_worker_ticks(1U);
    assert_last_applied_colors(false, false, false);
    assert(log_event_count(NRF_LOG_EVENT_SENSOR_BUS_FAULT) == 0U); /* nothing is reported before an init fails */
    assert(s_fake_faults[0].valid && s_fake_faults[1].valid);

    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    run_worker_until_empty();
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.activation_complete_generation == 1U);
    assert(!diagnostics.imu_ready && diagnostics.imu_init_result == -EIO);
    assert(!diagnostics.env_ready && diagnostics.env_init_result == -ENODEV);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_NO_DATA);
    assert(snapshot.sequence == 0U && snapshot.sample_complete_uptime_ms == 0U);
    assert(snapshot.valid_mask == 0U);
    assert(snapshot.fault_mask == (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
    assert(snapshot.flags == NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED);
    assert(s_refresh_calls == 0U && s_imu_read_calls == 0U);
    assert(log_event_count(NRF_LOG_EVENT_SENSOR_INIT_FAILED) == 2U);
    assert(s_log_events[0].event_id == NRF_LOG_EVENT_SENSOR_INIT_FAILED);
    assert(s_log_events[0].arg0 == NRF_SENSOR_VALID_IMU);
    assert(s_log_events[0].arg1 == (uint32_t)-EIO);
    /* Each init failure is followed by the cause of the transfers behind it, for that sensor only. */
    assert(log_event_count(NRF_LOG_EVENT_SENSOR_BUS_FAULT) == 2U);
    assert(s_log_events[1].event_id == NRF_LOG_EVENT_SENSOR_BUS_FAULT);
    assert(s_log_events[1].arg0 == NRF_SENSOR_VALID_IMU);
    assert(s_log_events[1].arg1 == imu_cause);
    const unsigned env_failed = find_log_event(NRF_LOG_EVENT_SENSOR_INIT_FAILED, NRF_SENSOR_VALID_ENV);
    assert(env_failed != UINT_MAX && s_log_events[env_failed].arg1 == (uint32_t)-ENODEV);
    assert(env_failed + 1U < s_log_event_count);
    assert(s_log_events[env_failed + 1U].event_id == NRF_LOG_EVENT_SENSOR_BUS_FAULT);
    assert(s_log_events[env_failed + 1U].arg0 == NRF_SENSOR_VALID_ENV);
    assert(s_log_events[env_failed + 1U].arg1 == env_cause);
    assert(!s_fake_faults[0].valid && !s_fake_faults[1].valid); /* taken exactly once */
    assert(diagnostics.logger_led_output.red);
    /* The red LED's cause reaches the log ring: BMI160 init failure latched at ERROR. */
    assert(log_event_count(NRF_LOG_EVENT_LOGGER_LATCH) == 1U);
    assert(diagnostics.logger_led_output.reason_source == NRF_LOGGER_LED_SOURCE_BMI160_FAULT);
    assert_last_log_event(
        NRF_LOG_EVENT_LOGGER_LATCH,
        NRF_LOGGER_LED_SOURCE_BMI160_FAULT,
        NRF_LOGGER_LED_SEVERITY_ERROR);
    assert_last_applied_colors(false, false, false);
    assert(signal_log_count_containing("signal: stage=SENSORS_FAILED ") == 1U);

    assert_stage_pattern(3U, false, false, true);
    assert_last_applied_colors(false, false, true);
    assert_last_applied_matches_policy();
    assert(signal_log_count_containing("signal: stage=SENSORS_FAILED ") == 1U);
    /* The same failure again is not a new event, so it must not report (or consume) a cause either. */
    s_fake_faults[0].valid = true;
    s_fake_faults[0].word = imu_cause;
    s_fake_faults[1].valid = true;
    s_fake_faults[1].word = env_cause;
    for (uint32_t id = 1U; id <= 3U; ++id) {
        const nrf_upstream_sensor_refresh_t request = {.request_id = id};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
        run_worker_until_empty();
    }
    assert(log_event_count(NRF_LOG_EVENT_SENSOR_INIT_FAILED) == 2U);
    assert(log_event_count(NRF_LOG_EVENT_SENSOR_BUS_FAULT) == 2U);
    assert(s_fake_faults[0].valid && s_fake_faults[1].valid);
}
#endif
#endif

NRF_TEST_MAYBE_UNUSED static void process_refresh_request(uint32_t request_id)
{
    nrf_upstream_sensor_refresh_t request = {.request_id = request_id};
    s_test_uptime_ms += 100;
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    run_worker_until_empty();
}

NRF_TEST_MAYBE_UNUSED static void process_height_reference_request(
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

NRF_TEST_MAYBE_UNUSED static void process_imu_calibration_request(
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
        assert(s_refresh_calls == refreshes_before && s_imu_read_calls <= reads_before + 1U);
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

#if defined(NRF_MAIN_LOGGER_TEST)
static void test_main_logger_without_remote_activation(void)
{
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};
    nrf_upstream_sensor_snapshot_t snapshot;
    const nrf_upstream_sensor_refresh_t refresh = {.request_id = 1U};

    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    run_worker_advancing(1000U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(!diagnostics.logger_led_output.green && !diagnostics.logger_led_output.red);
    nrf_sensor_upstream_runtime_zephyr_note_upstream(0U, 0U, NRF_UPSTREAM_ROLE_MAIN);
    run_worker_advancing(1000U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(!diagnostics.logger_led_output.green);

    nrf_sensor_upstream_runtime_zephyr_note_upstream(1U, 1U, NRF_UPSTREAM_ROLE_MAIN);
    run_worker_advancing(3000U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.logger_led_output.green);
    /* P1.07 is the red REMOTE alias, but the physical green LED on MAIN. */
    assert(s_led_samples[s_led_sample_count - 1U].red);
    assert(!s_led_samples[s_led_sample_count - 1U].green);
    assert(s_prepare_calls == 0U && s_refresh_calls == 0U && s_imu_read_calls == 0U);
    assert(diagnostics.activation_generation == 0U);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&refresh) == -EAGAIN);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_NOT_READY);

    s_led_counters.twis_protocol_error_total = 1U;
    run_worker_advancing(3000U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.logger_led_output.yellow);
    assert(s_led_samples[s_led_sample_count - 1U].yellow);
    /*
     * The ESP32 must be told which source latched, once, with the latched
     * severity. The harness re-enters the worker (which resets the policy) on
     * every run_worker_advancing(), so "once" is checked per call: a 3 s run
     * holds dozens of LED updates but records exactly one event.
     */
    assert(log_event_count(NRF_LOG_EVENT_LOGGER_LATCH) == 1U);
    assert_last_log_event(
        NRF_LOG_EVENT_LOGGER_LATCH,
        NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL,
        NRF_LOGGER_LED_SEVERITY_WARNING);
    s_led_counters.cs_ranging_failure_total = 1U;
    run_worker_advancing(1000U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.logger_led_output.red);
    assert(log_event_count(NRF_LOG_EVENT_LOGGER_LATCH) == 2U);
    assert_last_log_event(
        NRF_LOG_EVENT_LOGGER_LATCH,
        NRF_LOGGER_LED_SOURCE_CS_RANGING,
        NRF_LOGGER_LED_SEVERITY_ERROR);
    /* An equal-severity source added later neither replaces the reason nor adds an event. */
    s_led_counters.role_lost_total = 1U;
    run_worker_advancing(3000U);
    assert(log_event_count(NRF_LOG_EVENT_LOGGER_LATCH) == 3U);
    assert_last_log_event(
        NRF_LOG_EVENT_LOGGER_LATCH,
        NRF_LOGGER_LED_SOURCE_CS_RANGING,
        NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(s_led_samples[s_led_sample_count - 1U].green);
    assert(!s_led_samples[s_led_sample_count - 1U].red);
    assert(s_prepare_calls == 0U && s_refresh_calls == 0U && s_imu_read_calls == 0U);
}
#endif

int main(void)
{
#if defined(NRF_MAIN_LOGGER_TEST)
    test_main_logger_without_remote_activation();
    return 0;
#elif defined(NRF_REAL_SENSOR_WORKER_TEST)
    fake_sensor_bus_setup();
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t live_diagnostics;
    nrf_orientation_config_t config;
    nrf_orientation_default_config(&config);
    config.reference_axis = NRF_ORIENTATION_AXIS_POS_Z;
    config.sample_period_ms = 20U; /* exercise IMU reads just before ENV readiness */
    assert(nrf_sensor_upstream_runtime_zephyr_configure_orientation(&config) == 0);
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_sensor_snapshot_t snapshot;
    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    run_worker_until_empty();
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    const unsigned sleep_before = fake_sensor_bus_sleep_count;
    run_worker_advancing(100U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
    assert(live_diagnostics.imu_state == NRF_BMI160_MEASURING);
    assert(live_diagnostics.env_state == NRF_BME280_MEASURING);
    assert((fake_sensor_bus_imu_states & 7U) == 7U);
    assert((fake_sensor_bus_env_states & 7U) == 7U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(fake_sensor_bus_scan_reads == 0U);
    {
        const nrf_upstream_sensor_refresh_t scan = {.request_id = 900U};
        const nrf_upstream_sensor_refresh_t other_scan = {.request_id = 901U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_scan(&scan) == 0);
        assert(nrf_sensor_upstream_runtime_zephyr_request_scan(&scan) == 0);
        assert(nrf_sensor_upstream_runtime_zephyr_request_scan(&other_scan) == -EBUSY);
        run_worker_until_empty();
        assert(fake_sensor_bus_scan_reads == 0U);
    }
    run_worker_advancing(48U);
    assert(fake_sensor_bus_scan_reads < 112U);
    const unsigned imu_before_scan = fake_sensor_bus_imu_reads;
    run_worker_advancing(20U);
    {
        const nrf_upstream_sensor_refresh_t during_scan = {.request_id = 701U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&during_scan) == 0);
        run_worker_until_empty();
        const unsigned scan_before_wait = fake_sensor_bus_scan_reads;
        run_worker_advancing(46U);
        assert(fake_sensor_bus_scan_reads == scan_before_wait);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 701U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
        run_worker_advancing(2U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 701U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
    }
    run_worker_advancing(112U);
    assert(fake_sensor_bus_scan_reads == 112U);
    assert(fake_sensor_bus_imu_reads > imu_before_scan);
    assert(s_log_event_count >= 5U);
    {
        const unsigned first = s_log_event_count - 5U;
        for (unsigned i = 0U; i < 4U; ++i) {
            assert(s_log_events[first + i].event_id == NRF_LOG_EVENT_SENSOR_SCAN_WORD0 + i);
            assert(s_log_events[first + i].arg0 == 900U);
            assert(s_log_events[first + i].arg1 == (i == 3U ? ((1U << 8U) | (1U << 22U)) : 0U));
        }
        assert(s_log_events[first + 4U].event_id == NRF_LOG_EVENT_SENSOR_SCAN_DONE);
        assert(s_log_events[first + 4U].arg0 == 900U && s_log_events[first + 4U].arg1 == 2U);
    }
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
    assert((snapshot.valid_mask & (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV)) ==
           (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
    assert(snapshot.humidity_milli_percent == UINT32_MAX);
    assert(fake_sensor_bus_sleep_count == sleep_before);
    run_worker_advancing(75U);
    const unsigned imu_before = fake_sensor_bus_imu_reads;
    const nrf_upstream_sensor_refresh_t timing_req = {.request_id = 100U};
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&timing_req) == 0);
    run_worker_until_empty();
    run_worker_advancing(30U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(fake_sensor_bus_imu_reads > imu_before);
    run_worker_advancing(18U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 100U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
    const unsigned imu_before_load = fake_sensor_bus_imu_reads;
    for (uint32_t id = 1U; id <= 30U; ++id) {
        const nrf_upstream_sensor_refresh_t req = {.request_id = id};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&req) == 0);
        run_worker_until_empty();
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == id && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == id && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
        assert(snapshot.humidity_milli_percent == UINT32_MAX);
    }
    assert(fake_sensor_bus_sleep_count == sleep_before);
    assert(fake_sensor_bus_imu_reads >= imu_before_load + 30U);
    assert(fake_sensor_bus_imu_max_gap_ms <= 100U);
    {
        /* The worker can be descheduled after arming ENV while other tasks run. */
        const nrf_upstream_sensor_refresh_t delayed = {.request_id = 109U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&delayed) == 0);
        run_worker_until_empty();
        s_test_uptime_ms += 80;
        run_worker_advancing(1U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 109U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
        assert(snapshot.pressure_pa > 100000U && snapshot.pressure_pa < 101000U);
        assert(fake_sensor_bus_sleep_count == sleep_before);
    }
    {
        const nrf_upstream_height_reference_t height = {
            .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, .request_id = 110U, .generation = 11U,
        };
        assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    }
    {
        const nrf_upstream_sensor_refresh_t reset_req = {.request_id = 102U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&reset_req) == 0);
        run_worker_until_empty();
        fake_sensor_bus_reset_env();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 102U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_IMU) != 0U);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) == 0U);
        assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(!live_diagnostics.env_ready);
        fake_sensor_bus_hold_env_nvm(true);
        for (uint32_t id = 103U; id <= 104U; ++id) {
            const nrf_upstream_sensor_refresh_t retry_req = {.request_id = id};
            assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&retry_req) == 0);
            run_worker_until_empty();
        }
        const unsigned imu_before_env_retry = fake_sensor_bus_imu_reads;
        run_worker_advancing(150U);
        assert(fake_sensor_bus_imu_reads > imu_before_env_retry);
        assert(fake_sensor_bus_sleep_count == sleep_before);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(live_diagnostics.env_state == NRF_BME280_RECOVER);
        assert(live_diagnostics.imu_state == NRF_BMI160_MEASURING);
        fake_sensor_bus_hold_env_nvm(false);
        const nrf_upstream_sensor_refresh_t retry_scan = {.request_id = 901U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_scan(&retry_scan) == 0);
        run_worker_until_empty();
        run_worker_advancing(150U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(live_diagnostics.env_state == NRF_BME280_MEASURING);
    }
    fake_sensor_bus_set_bad_pressure();
    {
        const nrf_upstream_sensor_refresh_t req = {.request_id = 101U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&req) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 101U);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) == 0U);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_IMU) != 0U);
        assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
    }
    fake_sensor_bus_suspend_imu();
    for (unsigned wait = 0U; wait < 100U; ++wait) {
        run_worker_advancing(1U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        if (!live_diagnostics.imu_ready) break;
    }
    assert(!live_diagnostics.imu_ready);
    fake_sensor_bus_restore_pressure();
    {
        const nrf_upstream_sensor_refresh_t while_recovering = {.request_id = 2000U};
        const unsigned sleeps_during_recovery = fake_sensor_bus_sleep_count;
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&while_recovering) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 2000U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
        assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
        assert(fake_sensor_bus_sleep_count == sleeps_during_recovery);
        run_worker_advancing(100U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(live_diagnostics.imu_state == NRF_BMI160_MEASURING);
    }
    fake_sensor_bus_set_imu_present(false);
    for (uint32_t id = 2900U; id < 2903U; ++id) {
        const nrf_upstream_sensor_refresh_t req = {.request_id = id};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&req) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
    }
    run_worker_advancing(10U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
    assert(!live_diagnostics.imu_ready);
    assert(live_diagnostics.env_ready);
    {
        /* Real bus layer, real runtime: every cause event follows its init failure and decodes to what the fake did. */
        unsigned imu_causes = 0U;
        for (unsigned i = 0U; i < s_log_event_count; ++i) {
            if (s_log_events[i].event_id != NRF_LOG_EVENT_SENSOR_BUS_FAULT) continue;
            assert(i > 0U && s_log_events[i - 1U].event_id == NRF_LOG_EVENT_SENSOR_INIT_FAILED);
            assert(s_log_events[i - 1U].arg0 == s_log_events[i].arg0);
            nrf_sensor_bus_fault_t fault;
            assert(nrf_sensor_bus_fault_unpack(s_log_events[i].arg1, &fault));
            const bool imu = s_log_events[i].arg0 == NRF_SENSOR_VALID_IMU;
            assert(imu ? (fault.address == 0x68U || fault.address == 0x69U)
                       : (fault.address == 0x76U || fault.address == 0x77U));
            assert(fault.cause == NRF_SENSOR_BUS_ERR_TRANSFER && fault.errno_abs == (uint8_t)ENXIO);
            assert(fault.lines_valid && fault.sda_high && fault.scl_high && fault.failures >= 1U);
            if (imu) ++imu_causes;
        }
        assert(imu_causes >= 1U); /* the removed IMU produced a cause, not only a driver code */
    }
    {
        const unsigned probes_after_failure = fake_sensor_bus_imu_id_probes;
        for (uint32_t id = 3000U; id < 3100U; ++id) {
            const nrf_upstream_sensor_refresh_t req = {.request_id = id};
            assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&req) == 0);
            run_worker_until_empty();
            run_worker_advancing(48U);
            assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
            assert(snapshot.request_id == id && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
            assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
        }
        assert(fake_sensor_bus_imu_id_probes == probes_after_failure);
        fake_sensor_bus_set_imu_present(true);
        run_worker_advancing(500U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(live_diagnostics.imu_state == NRF_BMI160_MEASURING && live_diagnostics.imu_ready);
        assert(fake_sensor_bus_sleep_count == sleep_before);
        const nrf_upstream_sensor_refresh_t fresh = {.request_id = 3100U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&fresh) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 3100U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_OK);
        assert((snapshot.valid_mask & (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV)) ==
               (NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV));
    }
    {
        const nrf_upstream_height_reference_t reference = {
            .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, .request_id = 3120U, .generation = 12U,
        };
        assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&reference) == 0);
        run_worker_until_empty();
        run_worker_advancing(48U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
        fake_sensor_bus_set_drdy_mask(0x40U); /* GYRO-only cannot keep a reference alive */
        for (uint32_t id = 3121U; id <= 3128U; ++id) {
            const nrf_upstream_sensor_refresh_t pending = {.request_id = id};
            assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&pending) == 0);
            run_worker_until_empty();
            run_worker_advancing(48U);
            assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
            assert(snapshot.request_id == id && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PARTIAL);
            assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
        }
        assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
        fake_sensor_bus_set_drdy_mask(0xC0U);
        run_worker_advancing(150U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&live_diagnostics));
        assert(live_diagnostics.imu_ready && live_diagnostics.env_ready);
    }
    return 0;
#elif defined(NRF_ASYNC_ACTIVATION_TEST)
    s_include_env = true;
    s_async_env = true;
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_sensor_snapshot_t snapshot;
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics = {0};
    assert(nrf_sensor_upstream_runtime_zephyr_start() == 0);
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    run_worker_until_empty();
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.activation_generation == 1U && diagnostics.activation_complete_generation == 0U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    run_worker_advancing(46U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.activation_complete_generation == 0U);
    run_worker_advancing(2U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics));
    assert(diagnostics.activation_complete_generation == 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U && snapshot.result != NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
    return 0;
#elif defined(NRF_STAGE_SIGNAL_FAILED_TEST)
    test_failed_sensor_signal();
    return 0;
#elif defined(NRF_STAGE_SIGNAL_SUCCESS_TEST)
    test_successful_stage_signals();
    return 0;
#else
    nrf_orientation_config_t orientation_config;
    nrf_orientation_output_t orientation;
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_assign_t conflicting = assign;
    nrf_upstream_sensor_refresh_t request = {.request_id = 20U};
    nrf_upstream_sensor_snapshot_t snapshot;
    const unsigned queue_calls_base = s_queue_put_calls;

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

    /* A full queue returns the real put failure and rolls activation back. */
    fill_sensor_queue_and_check_capacity();
    assert(s_queue_put_calls == queue_calls_base + 5U);
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == -ENOSPC);
    assert(s_queue_put_calls == queue_calls_base + 6U);
    k_msgq_purge(&s_sensor_queue);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_NOT_READY);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == -EAGAIN);
    assert(s_queue_put_calls == queue_calls_base + 6U);

    /*
     * Successful notification commits logical REMOTE activation synchronously,
     * before the worker performs any private-sensor I/O. An immediate external
     * refresh therefore observes request_id=0 PENDING/BUSY, never NOT_REMOTE.
     */
    assert(nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(&assign) == 0);
    /* The worker must use the queue's copy after this caller-owned value changes. */
    assign.request_id = 0xBADU;
    assign.node_id = 0x77U;
    assign.mux_channel = 0x66U;
    assign.role = NRF_UPSTREAM_ROLE_MAIN;
    assert(s_queue_put_calls == queue_calls_base + 7U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 0U);
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == -EBUSY);
    assert(s_queue_put_calls == queue_calls_base + 7U);
    {
        const unsigned queue_calls_before = s_queue_put_calls;
        const nrf_upstream_height_reference_t height = {
            .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE,
            .request_id = 18U,
            .generation = 18U,
        };
        const nrf_upstream_imu_calibrate_t calibration = {
            .action = NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE,
            .request_id = 19U,
            .generation = 19U,
        };

        assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height) == -EBUSY);
        assert(nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(&calibration) == -EBUSY);
        assert(s_queue_put_calls == queue_calls_before);
    }
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
    assign = make_assign();

    /* A live system probe cannot make queue rejection look like sensor success. */
    {
        const unsigned prepare_calls_before = s_prepare_calls;
        const unsigned refresh_calls_before = s_refresh_calls;
        const unsigned imu_reads_before = s_imu_read_calls;
        nrf_upstream_height_reference_t height = {
            .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE,
            .request_id = 101U,
            .generation = 101U,
        };
        nrf_upstream_imu_calibrate_t calibration = {
            .action = NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE,
            .request_id = 102U,
            .generation = 102U,
        };

        fill_sensor_queue_and_check_capacity();
        assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(
                   &(nrf_upstream_sensor_refresh_t){.request_id = 100U}) == -ENOSPC);
        k_msgq_purge(&s_sensor_queue);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == 100U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_BUSY);

        fill_sensor_queue_and_check_capacity();
        assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&height) == -ENOSPC);
        k_msgq_purge(&s_sensor_queue);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == height.request_id &&
               snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_BUSY);

        fill_sensor_queue_and_check_capacity();
        assert(nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(&calibration) == -ENOSPC);
        k_msgq_purge(&s_sensor_queue);
        assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
        assert(snapshot.request_id == calibration.request_id &&
               snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_BUSY);
        assert(s_prepare_calls == prepare_calls_before);
        assert(s_refresh_calls == refresh_calls_before);
        assert(s_imu_read_calls == imu_reads_before);
    }

#if !defined(CONFIG_APP_NRF_STAGE_SIGNALS) || !CONFIG_APP_NRF_STAGE_SIGNALS
    assert_last_applied_matches_policy();
#endif

    /* The explicit request is accepted after the activation sample completes. */
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&request) == 0);
    assert(s_queue_put_calls == queue_calls_base + 26U);

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
    assert(log_event_count(NRF_LOG_EVENT_HEIGHT_REF_REJECTED) == 1U);
    assert_last_log_event(
        NRF_LOG_EVENT_HEIGHT_REF_REJECTED,
        44U,
        NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    process_height_reference_request(
        NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 0x44U + 1U, 43U);
    assert(log_event_count(NRF_LOG_EVENT_HEIGHT_REF_REJECTED) == 1U);

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

    s_force_stationary = false;
    s_force_moving = true;
    process_imu_calibration_request(
        NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE, 63U, 63U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
    assert(log_event_count(NRF_LOG_EVENT_IMU_CAL_REJECTED) == 1U);
    assert_last_log_event(
        NRF_LOG_EVENT_IMU_CAL_REJECTED,
        63U,
        NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);

    s_force_moving = false;
    s_force_stationary = true;
    s_include_env = true;
    s_test_fault_mask = NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV;
    process_refresh_request(70U);
    assert(log_event_count(NRF_LOG_EVENT_IMU_FAULT_SET) == 1U);
    assert(log_event_count(NRF_LOG_EVENT_ENV_FAULT_SET) == 1U);
    process_refresh_request(71U);
    assert(log_event_count(NRF_LOG_EVENT_IMU_FAULT_SET) == 1U);
    assert(log_event_count(NRF_LOG_EVENT_ENV_FAULT_SET) == 1U);

    s_test_fault_mask = 0U;
    process_refresh_request(72U);
    assert(log_event_count(NRF_LOG_EVENT_IMU_FAULT_CLEARED) == 1U);
    assert(log_event_count(NRF_LOG_EVENT_ENV_FAULT_CLEARED) == 1U);
    process_refresh_request(73U);
    assert(log_event_count(NRF_LOG_EVENT_IMU_FAULT_CLEARED) == 1U);
    assert(log_event_count(NRF_LOG_EVENT_ENV_FAULT_CLEARED) == 1U);

    /* Lose the BMI160 during periodic orientation, with both references established. */
    s_force_stationary = true;
    s_force_moving = false;
    process_imu_calibration_request(NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE, 1000U, 100U);
    process_height_reference_request(NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 1001U, 101U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    s_fail_imu_reads = true;
    s_fail_imu_init = true;
    const unsigned before_fail_reads = s_imu_read_calls;
    run_worker_advancing(600U);
    assert(s_imu_read_calls == before_fail_reads + NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES);
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t failed_diag = {0};
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&failed_diag));
    assert(!failed_diag.imu_ready);
    {
        const unsigned attempts_before_scan = s_prepare_calls;
        const nrf_upstream_sensor_refresh_t scan = {.request_id = 7000U};
        assert(nrf_sensor_upstream_runtime_zephyr_request_scan(&scan) == 0);
        run_worker_until_empty();
        assert(s_prepare_calls == attempts_before_scan + 1U);
        assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&failed_diag));
        assert(!failed_diag.imu_ready && failed_diag.env_ready);
    }
    s_fail_imu_reads = false;
    s_fail_imu_init = false;
    const unsigned attempts_after_loss = s_prepare_calls;
    run_worker_advancing(NRF_REMOTE_SENSOR_RETRY_MS - 1000U);
    assert(s_prepare_calls == attempts_after_loss);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&failed_diag));
    assert(!failed_diag.imu_ready);
    run_worker_advancing(1500U);
    assert(s_prepare_calls == attempts_after_loss + 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&failed_diag));
    assert(failed_diag.imu_ready);
    process_refresh_request(1002U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state != NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(snapshot.gyro_bias_mdps[0] == 0 && snapshot.gravity_ref_mg[0] == 0);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(snapshot.relative_z_mm == 0);

    /* Losing the barometer invalidates height but preserves the healthy IMU calibration. */
    process_imu_calibration_request(NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE, 1003U, 102U);
    process_height_reference_request(NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 1004U, 103U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    s_test_fault_mask = NRF_SENSOR_VALID_ENV;
    process_refresh_request(1005U);
    process_refresh_request(1006U);
    process_refresh_request(1007U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    s_test_fault_mask = 0U;
    process_refresh_request(1008U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(snapshot.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);

    /* A fresh environmental sample is pending without blocking periodic IMU reads. */
    run_worker_advancing(70U);
    s_async_env = true;
    const nrf_upstream_sensor_refresh_t async_request = { .request_id = 2001U };
    const unsigned async_refreshes = s_refresh_calls;
    const unsigned async_imu_reads = s_imu_read_calls;
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(&async_request) == 0);
    run_worker_until_empty();
    assert(s_refresh_calls == async_refreshes);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING && snapshot.request_id == 2001U);
    assert(nrf_sensor_upstream_runtime_zephyr_request_refresh(
        &(nrf_upstream_sensor_refresh_t){.request_id = 2002U}) == -EBUSY);
    run_worker_advancing(46U);
    assert(s_refresh_calls == async_refreshes);
    assert(s_imu_read_calls > async_imu_reads);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    run_worker_advancing(2U);
    assert(s_refresh_calls == async_refreshes + 1U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 2001U && snapshot.result != NRF_UPSTREAM_SENSOR_RESULT_BUSY);
    assert((snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
    const nrf_upstream_height_reference_t async_height = {
        .action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, .request_id = 2003U, .generation = 200U,
    };
    assert(nrf_sensor_upstream_runtime_zephyr_request_height_reference(&async_height) == 0);
    run_worker_until_empty();
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 2003U && snapshot.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING);
    run_worker_advancing(48U);
    assert(nrf_sensor_upstream_runtime_zephyr_get_snapshot(&snapshot));
    assert(snapshot.request_id == 2003U && snapshot.reference_generation == 200U);
    assert(snapshot.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    s_async_env = false;

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
#endif
}
