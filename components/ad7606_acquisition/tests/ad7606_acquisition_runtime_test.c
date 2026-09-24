#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "ad7606_power_policy.h"
#include "app_task_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ina3221_monitor.h"
#include "runtime_observability.h"
#include "seismic_buffer.h"

/*
 * MAX-246 item 2: ad7606_acquisition start/stop lifecycle, timer, sample
 * path and the MAX-242 rail interlock, without a board.
 *
 * The Core 1 worker runs inline inside xTaskCreatePinnedToCore. Its
 * ulTaskNotifyTake() consumes a script: TICK events advance the clock by
 * whole sampling periods and return the notification count; a STOP event
 * calls the real ad7606_acquisition_stop(), whose wait loop (vTaskDelay
 * while the worker still exists) jumps back into the worker so it can
 * observe the stop and clean up, as the real scheduler would.
 */

#define PERIOD_US 2000LL /* 500 Hz */

typedef enum { EV_TICK, EV_STOP } event_type_t;
typedef struct { event_type_t type; uint32_t count; } event_t;

static event_t s_script[64];
static size_t s_script_len;
static size_t s_script_pos;

static int64_t s_now_us;
static int s_critical_depth;
static bool s_in_worker;
static bool s_stop_waiting;
static jmp_buf s_stop_return;
static int s_task_token;

/* Recorded behaviour. */
static unsigned s_task_creates;
static BaseType_t s_task_core;
static UBaseType_t s_task_priority;
static unsigned s_driver_inits;
static bool s_driver_init_in_worker;
static unsigned s_driver_deinits;
static unsigned s_reads;
static unsigned s_reads_after_deinit;
static bool s_driver_live;
static unsigned s_pushes;
static int64_t s_last_push_ts;
static float s_last_push[SEISMIC_CHANNEL_COUNT];
static seismic_source_t s_buffer_source;
static uint32_t s_buffer_rate;
static uint32_t s_ad7606_rate;
static bool s_running_seen_during_push;
static unsigned s_beats;
static bool s_timer_exists;
static bool s_timer_started;
static uint64_t s_timer_period;
static esp_timer_dispatch_t s_timer_dispatch;
static esp_timer_cb_t s_timer_cb;
static unsigned s_notify_gives;

/* Injected behaviour. */
static esp_err_t s_driver_init_result;
static esp_err_t s_timer_create_result;
static esp_err_t s_timer_start_result;
static esp_err_t s_read_result;
static bool s_rail_available;
static bool s_rail_valid;
static float s_rail_v;
static bool s_rail_fresh;
/* Rail reads s_rail_low_v inside [low_from, low_until). */
static int64_t s_rail_low_from_us;
static int64_t s_rail_low_until_us;
static float s_rail_low_v;

static void reset_world(void)
{
    s_script_len = 0U;
    s_script_pos = 0U;
    s_now_us = 50000000LL;
    s_critical_depth = 0;
    s_in_worker = false;
    s_stop_waiting = false;
    s_task_creates = 0U;
    s_driver_inits = 0U;
    s_driver_init_in_worker = false;
    s_driver_deinits = 0U;
    s_reads = 0U;
    s_reads_after_deinit = 0U;
    s_driver_live = false;
    s_pushes = 0U;
    s_buffer_source = SEISMIC_SOURCE_TEST;
    s_buffer_rate = 12345U;
    s_ad7606_rate = 0U;
    s_running_seen_during_push = true;
    s_beats = 0U;
    s_timer_exists = false;
    s_timer_started = false;
    s_timer_period = 0U;
    s_timer_cb = NULL;
    s_notify_gives = 0U;
    s_driver_init_result = ESP_OK;
    s_timer_create_result = ESP_OK;
    s_timer_start_result = ESP_OK;
    s_read_result = ESP_OK;
    s_rail_available = true;
    s_rail_valid = true;
    s_rail_v = 5.02f;
    s_rail_fresh = true;
    s_rail_low_from_us = INT64_MAX;
    s_rail_low_until_us = INT64_MAX;
    s_rail_low_v = 0.0f;
}

static void script(event_type_t type, uint32_t count)
{
    assert(s_script_len < sizeof(s_script) / sizeof(s_script[0]));
    s_script[s_script_len].type = type;
    s_script[s_script_len].count = count;
    ++s_script_len;
}

/* ---- FreeRTOS ---- */

void test_port_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 0);
    ++s_critical_depth;
}

void test_port_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 1);
    --s_critical_depth;
}

struct test_semaphore { int given; };
static struct test_semaphore s_sem;
static bool s_sem_live;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    assert(!s_sem_live);
    s_sem_live = true;
    s_sem.given = 0;
    return &s_sem;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_sem && s_sem_live);
    s_sem.given = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &s_sem && s_sem_live);
    assert(ticks == portMAX_DELAY);
    /* The worker already ran inline: start() must only ever wait for a
     * semaphore the worker actually gave. */
    assert(s_sem.given == 1);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_sem && s_sem_live);
    s_sem_live = false;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t stack_depth, void *parameters,
    UBaseType_t priority, TaskHandle_t *created_task, BaseType_t core_id)
{
    (void)name;
    (void)stack_depth;
    ++s_task_creates;
    s_task_core = core_id;
    s_task_priority = priority;
    /* FreeRTOS publishes the handle before the task first runs. */
    *created_task = &s_task_token;
    s_in_worker = true;
    entry(parameters);
    s_in_worker = false;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    assert(s_in_worker);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return &s_task_token;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &s_task_token);
    ++s_notify_gives;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_critical_depth == 0);
    (void)ticks;
    if (s_stop_waiting) {
        /* stop() is waiting for the worker: let the worker run. */
        s_stop_waiting = false;
        longjmp(s_stop_return, 1);
    }
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    assert(clear_on_exit == pdTRUE);
    assert(ticks_to_wait == portMAX_DELAY);
    assert(s_in_worker);
    assert(s_script_pos < s_script_len); /* every test ends with STOP or self-stop */
    const event_t event = s_script[s_script_pos++];
    if (event.type == EV_TICK) {
        s_now_us += PERIOD_US * (int64_t)event.count;
        return event.count;
    }
    if (setjmp(s_stop_return) == 0) {
        s_stop_waiting = true;
        ad7606_acquisition_stop();
        assert(false); /* stop() must wait for the worker, not return */
    }
    return 1U;
}

/* ---- esp_timer ---- */

struct test_esp_timer { int unused; };
static struct test_esp_timer s_timer_obj;

int64_t esp_timer_get_time(void) { return s_now_us; }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle)
{
    assert(s_in_worker);
    if (s_timer_create_result != ESP_OK) {
        return s_timer_create_result;
    }
    assert(!s_timer_exists);
    s_timer_exists = true;
    s_timer_cb = args->callback;
    s_timer_dispatch = args->dispatch_method;
    *out_handle = &s_timer_obj;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    assert(timer == &s_timer_obj && s_timer_exists);
    if (s_timer_start_result != ESP_OK) {
        return s_timer_start_result;
    }
    s_timer_started = true;
    s_timer_period = period_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &s_timer_obj && s_timer_exists);
    s_timer_started = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &s_timer_obj && s_timer_exists);
    assert(!s_timer_started); /* stopped before delete */
    s_timer_exists = false;
    return ESP_OK;
}

/* ---- AD7606 driver ---- */

esp_err_t ad7606_driver_init(void)
{
    ++s_driver_inits;
    s_driver_init_in_worker = s_in_worker;
    if (s_driver_init_result != ESP_OK) {
        return s_driver_init_result;
    }
    s_driver_live = true;
    return ESP_OK;
}

void ad7606_driver_deinit(void)
{
    ++s_driver_deinits;
    s_driver_live = false;
}

static int16_t raw_for(unsigned sample, unsigned channel)
{
    return (int16_t)((int)(sample * 10U + channel) - 40);
}

esp_err_t ad7606_driver_read(ad7606_sample_t *sample)
{
    ++s_reads;
    if (!s_driver_live) {
        ++s_reads_after_deinit;
    }
    if (s_read_result != ESP_OK) {
        return s_read_result;
    }
    sample->timestamp_us = s_now_us - 7;
    for (unsigned c = 0U; c < AD7606_CHANNEL_COUNT; ++c) {
        sample->raw[c] = raw_for(s_reads, c);
    }
    return ESP_OK;
}

esp_err_t ad7606_driver_get_last_serial_observation(ad7606_serial_observation_t *observation)
{
    memset(observation, 0, sizeof(*observation));
    observation->conversion_complete = true;
    observation->spi_transaction_ok = true;
    for (unsigned c = 0U; c < AD7606_CHANNEL_COUNT; ++c) {
        const uint16_t word = (uint16_t)raw_for(s_reads, c);
        observation->rx[2U * c] = (uint8_t)(word >> 8);
        observation->rx[2U * c + 1U] = (uint8_t)(word & 0xFFU);
    }
    return ESP_OK;
}

float ad7606_raw_to_uv(int16_t raw)
{
    return (float)raw * 152.587890625f; /* 5 V / 32768 */
}

/* ---- INA3221 CH3 rail ---- */

esp_err_t ina3221_monitor_get_rail(size_t channel_index, ina3221_monitor_rail_t *rail)
{
    assert(channel_index == INA3221_MONITOR_RAIL_AD7606_5V);
    memset(rail, 0, sizeof(*rail));
    rail->available = s_rail_available;
    rail->valid = s_rail_valid;
    rail->bus_voltage_v =
        (s_now_us >= s_rail_low_from_us && s_now_us < s_rail_low_until_us) ? s_rail_low_v : s_rail_v;
    /* Each reading is a distinct conversion stamped "now" when fresh. */
    rail->sampled_at_us = s_rail_fresh ? s_now_us : s_now_us - 10000000LL;
    return ESP_OK;
}

/* ---- seismic buffer / observability ---- */

esp_err_t seismic_buffer_configure(seismic_source_t source, uint32_t sample_rate_hz)
{
    s_buffer_source = source;
    s_buffer_rate = sample_rate_hz;
    if (source == SEISMIC_SOURCE_AD7606) {
        s_ad7606_rate = sample_rate_hz;
    }
    return ESP_OK;
}

esp_err_t seismic_buffer_push(int64_t timestamp_us, const float channel_uv[SEISMIC_CHANNEL_COUNT])
{
    ++s_pushes;
    s_last_push_ts = timestamp_us;
    memcpy(s_last_push, channel_uv, sizeof(s_last_push));
    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    s_running_seen_during_push = s_running_seen_during_push && status.running;
    assert(s_buffer_source == SEISMIC_SOURCE_AD7606);
    return ESP_OK;
}

void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress)
{
    assert(task == RUNTIME_TASK_ACQUISITION);
    (void)progress;
    ++s_beats;
}

/* ---- helpers ---- */

static ad7606_acquisition_status_t status_now(void)
{
    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    return status;
}

static void assert_fully_released(void)
{
    assert(!s_timer_exists);
    assert(!s_driver_live);
    assert(s_buffer_source == SEISMIC_SOURCE_NONE);
    assert(!status_now().running);
    assert(!s_sem_live);
}

/* ---- tests ---- */

static void test_run_samples_then_stop_releases_everything(void)
{
    reset_world();
    script(EV_TICK, 1U);
    script(EV_TICK, 1U);
    script(EV_TICK, 1U);
    script(EV_STOP, 0U);

    assert(ad7606_acquisition_start() == ESP_OK);

    /* Worker on the real-time core; SPI/driver brought up there. */
    assert(s_task_creates == 1U);
    assert(s_task_core == APP_TASK_CORE_REALTIME);
    assert(s_task_priority == APP_TASK_PRIORITY_AD7606);
    assert(s_driver_inits == 1U && s_driver_init_in_worker);
    assert(s_timer_dispatch == ESP_TIMER_TASK);
    assert(s_timer_period == (uint64_t)PERIOD_US);
    assert(s_ad7606_rate == 500U);
    assert(s_buffer_rate == 0U); /* detached again on stop */

    /* Three periods -> three conversions -> three pushes of channels 1..4. */
    assert(s_reads == 3U);
    assert(s_pushes == 3U);
    assert(s_running_seen_during_push);
    assert(s_last_push_ts == s_now_us - 7);
    for (unsigned c = 0U; c < SEISMIC_CHANNEL_COUNT; ++c) {
        assert(fabsf(s_last_push[c] - ad7606_raw_to_uv(raw_for(3U, c))) < 1e-3f);
    }
    assert(s_beats >= 3U);

    /* STOP: timer stopped+deleted, driver deinit (-> Hi-Z safe state),
     * buffer detached, status not running. */
    assert(s_driver_deinits == 1U);
    assert(s_reads_after_deinit == 0U);
    assert_fully_released();

    const ad7606_acquisition_status_t status = status_now();
    assert(status.samples == 3U);
    assert(status.read_errors == 0U);
    assert(status.power_gate == AD7606_POWER_GATE_OK);
    assert(!status.power_fault_latched);

    /* A late timer callback after stop never wakes a dead task. */
    const unsigned gives = s_notify_gives;
    s_timer_cb(NULL);
    assert(s_notify_gives == gives);
}

static void test_collapsed_periods_are_one_conversion(void)
{
    reset_world();
    script(EV_TICK, 4U); /* worker was late: 4 periods in one wake */
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(s_reads == 1U);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.timer_ticks == 4U);
    assert(status.conversion_attempts == 1U);
    assert(status.collapsed_periods == 3U);
}

static void test_low_rail_refuses_start_without_touching_the_adc(void)
{
    reset_world();
    s_rail_v = 4.60f; /* fresh reading below the 4.75 V start minimum */
    assert(ad7606_acquisition_start() == ESP_ERR_INVALID_STATE);
    assert(s_task_creates == 0U);
    assert(s_driver_inits == 0U);
    assert(!s_timer_exists);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.power_gate == AD7606_POWER_GATE_RAIL_LOW);
    assert(status.power_fault_latched);
    assert(fabsf(status.power_fault_rail_v - 4.60f) < 1e-6f);
    assert(!status.running);
}

static void test_unverified_rail_starts_with_warning(void)
{
    const int cases = 3;
    for (int i = 0; i < cases; ++i) {
        reset_world();
        if (i == 0) { s_rail_available = false; s_rail_v = 0.0f; }
        if (i == 1) { s_rail_valid = false; s_rail_v = 0.0f; }
        if (i == 2) { s_rail_fresh = false; s_rail_v = 0.0f; }
        script(EV_TICK, 1U);
        script(EV_STOP, 0U);
        assert(ad7606_acquisition_start() == ESP_OK);
        const ad7606_acquisition_status_t status = status_now();
        assert(status.power_gate == AD7606_POWER_GATE_UNVERIFIED);
        assert(!status.power_fault_latched);
        assert(s_reads == 1U);
    }
}

static void test_confirmed_rail_loss_stops_and_releases_the_adc(void)
{
    reset_world();
    const int64_t t0 = s_now_us;
    /* The rail collapses 1.5 s into the run. The worker checks it at most
     * once per second; two fresh low readings confirm the loss. */
    s_rail_low_from_us = t0 + 1500000LL;
    s_rail_low_v = 4.20f;
    for (int i = 0; i < 40; ++i) {
        script(EV_TICK, 100U); /* 0.2 s per wake */
    }
    /* No STOP event: the worker must stop by itself. */
    assert(ad7606_acquisition_start() == ESP_OK);

    const ad7606_acquisition_status_t status = status_now();
    assert(status.power_fault_latched);
    assert(fabsf(status.power_fault_rail_v - 4.20f) < 1e-6f);
    assert(status.power_gate == AD7606_POWER_GATE_RAIL_LOW);
    assert(s_driver_deinits == 1U);   /* deinit -> Hi-Z safe state */
    assert(s_reads_after_deinit == 0U);
    assert_fully_released();
    /* Loss at 1.5 s; low checks at ~2.2 s and ~3.2 s -> stopped at the
     * second one, long before the script ran out. */
    assert(s_script_pos < s_script_len);
    assert(s_now_us - t0 >= 2000000LL && s_now_us - t0 <= 3400000LL);
    /* The confirming wake does not convert any more. */
    assert(s_reads == (unsigned)s_script_pos - 1U);
}

static void test_single_low_reading_does_not_stop(void)
{
    reset_world();
    const int64_t t0 = s_now_us;
    /* One rail check (and only one) lands in a short dip. */
    s_rail_low_from_us = t0 + 1500000LL;
    s_rail_low_until_us = t0 + 2500000LL;
    s_rail_low_v = 4.20f;
    for (int i = 0; i < 25; ++i) {
        script(EV_TICK, 100U);
    }
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(s_script_pos == s_script_len); /* ran until the explicit STOP */
    assert(!status_now().power_fault_latched);
    assert(s_reads == 25U);
}

static void test_read_errors_are_counted_and_not_pushed(void)
{
    reset_world();
    s_read_result = ESP_ERR_TIMEOUT;
    script(EV_TICK, 1U);
    script(EV_TICK, 1U);
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(s_reads == 2U);
    assert(s_pushes == 0U);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.read_errors == 2U);
    assert(status.consecutive_failures == 2U);
    assert(status.samples == 0U);
    assert(s_beats >= 2U); /* still alive for the watchdog */
    assert_fully_released();
}

static void test_startup_failures_clean_up(void)
{
    /* Driver init, timer create and timer start failures: start() reports
     * the error and nothing stays configured or driven. */
    for (int which = 0; which < 3; ++which) {
        reset_world();
        const esp_err_t injected = which == 0 ? ESP_ERR_NOT_FOUND
            : which == 1 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
        if (which == 0) s_driver_init_result = injected;
        if (which == 1) s_timer_create_result = injected;
        if (which == 2) s_timer_start_result = injected;
        assert(ad7606_acquisition_start() == injected);
        assert(s_driver_deinits == 1U);
        assert(s_reads == 0U);
        assert(s_script_pos == 0U);
        assert_fully_released();
    }
}

static void test_restart_clears_a_latched_fault(void)
{
    reset_world();
    s_rail_v = 4.10f;
    assert(ad7606_acquisition_start() == ESP_ERR_INVALID_STATE);
    assert(status_now().power_fault_latched);

    reset_world();
    script(EV_TICK, 1U);
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(!status_now().power_fault_latched);
    /* stop() on an idle acquisition is harmless. */
    ad7606_acquisition_stop();
    assert(!status_now().running);
}

int main(void)
{
    test_run_samples_then_stop_releases_everything();
    test_collapsed_periods_are_one_conversion();
    test_low_rail_refuses_start_without_touching_the_adc();
    test_unverified_rail_starts_with_warning();
    test_confirmed_rail_loss_stops_and_releases_the_adc();
    test_single_low_reading_does_not_stop();
    test_read_errors_are_counted_and_not_pushed();
    test_startup_failures_clean_up();
    test_restart_clears_a_latched_fault();
    return 0;
}
