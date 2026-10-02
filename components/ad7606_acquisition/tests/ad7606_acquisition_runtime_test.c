#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "ad7606_power_policy.h"
#include "sdkconfig.h"
#include "../ad7606_diag_accum.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_rom_sys.h"
#include "app_task_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ina3221_monitor.h"
#include "runtime_observability.h"
#include "seismic_buffer.h"

_Static_assert(
    sizeof(((ad7606_diag_channel_t *)0)->sum_sq_raw) == 8U,
    "AD7606 diagnostic sum of squares must be 64 bits"
);
_Static_assert(
    _Generic(
        ((ad7606_diag_channel_t *)0)->sum_sq_raw,
        uint64_t: 1,
        default: 0
    ),
    "AD7606 diagnostic sum of squares must be uint64_t"
);

/*
 * MAX-246 item 2: AD7606 hardware-timed lifecycle, indexed samples
 * path and the MAX-242 rail interlock, without a board.
 *
 * The Core 1 worker runs inline inside xTaskCreatePinnedToCore. Its
 * ulTaskNotifyTake() consumes a script: TICK events generate BUSY edges,
 * advance the PCNT conversion counter and return coalesced notifications; STOP
 * calls the real ad7606_acquisition_stop(), whose wait loop (vTaskDelay
 * while the worker still exists) jumps back into the worker so it can
 * observe the stop and clean up, as the real scheduler would.
 */

#define PERIOD_US 2000LL /* 500 Hz */
#define TEST_EVENT_CAPACITY \
    (CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ + 64U)
#define TEST_LOG_RECORD_CAPACITY 32U

typedef enum { EV_TICK, EV_STOP } event_type_t;
typedef struct {
    event_type_t type;
    uint32_t count;
    int64_t timestamp_adjustment_us;
} event_t;

static event_t s_script[TEST_EVENT_CAPACITY];
static size_t s_script_len;
static size_t s_script_pos;

static int64_t s_now_us;
static unsigned s_esp_timer_get_time_calls;
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
static int64_t s_push_timestamps[64];
static uint64_t s_push_sample_indices[64];
static int64_t s_last_push_ts;
static uint64_t s_last_push_sample_index;
static int64_t s_last_push_wall_time_us;
static uint32_t s_fake_serial_valid_after_conversion;
static uint32_t s_fake_serial_clocklike_from_conversion;
static uint32_t s_fake_serial_fixed_from_conversion;
static uint32_t s_pending_notifications;
static bool s_synthetic_log_delay;
static int64_t s_worker_iteration_start_us;
static bool s_worker_iteration_active;
static bool s_worker_iteration_published;
static uint64_t s_worker_iteration_sample_index;
static uint64_t s_worker_iteration_duration_us[TEST_EVENT_CAPACITY];
static bool s_worker_iteration_duration_recorded[TEST_EVENT_CAPACITY];
static int64_t s_push_wall_by_sample[TEST_EVENT_CAPACITY];
static int64_t s_push_timestamp_by_sample[TEST_EVENT_CAPACITY];
static bool s_push_recorded_by_sample[TEST_EVENT_CAPACITY];
static uint32_t s_last_synthetic_notification_batch;
static uint32_t s_max_synthetic_notification_batch;
static float s_last_push[SEISMIC_CHANNEL_COUNT];
static seismic_source_t s_buffer_source;
static uint32_t s_buffer_rate;
static uint32_t s_ad7606_rate;
static bool s_running_seen_during_push;
static unsigned s_beats;
static unsigned s_notify_gives;
static unsigned s_notify_from_isr;
static unsigned s_busy_isr_calls;
static unsigned s_stop_isr_suppressions;
static unsigned s_task_blocking_calls;
static uint32_t s_fake_pcnt_count;
static int s_fake_busy_level;
static gpio_int_type_t s_fake_busy_intr_type;
static gpio_int_type_t s_last_busy_intr_edge;
static gpio_isr_t s_fake_busy_isr;
static void *s_fake_busy_isr_args;
static bool s_fake_gpio_intr_enabled;
static bool s_fake_pcnt_enabled;
static bool s_fake_pcnt_started;
static bool s_fake_ledc_timer_configured;
static bool s_fake_ledc_channel_configured;
static uint32_t s_fake_ledc_frequency;
static uint32_t s_last_ledc_frequency;
static uint32_t s_last_ledc_duty;
static uint32_t s_last_ledc_resolution;
static int s_fake_gpio_install_flags;
static unsigned s_fake_watch_points;

/* Injected behaviour. */
static esp_err_t s_driver_init_result;
static uint32_t s_fake_measured_tconv_ns;
static esp_err_t s_gpio_install_result;
static bool s_fake_isr_service_ready;
static bool s_inject_busy_isr_on_stop;
static esp_err_t s_ledc_timer_result;
static esp_err_t s_read_result;
static bool s_rail_available;
static bool s_rail_valid;
static float s_rail_v;
static bool s_rail_fresh;
/* Rail reads s_rail_low_v inside [low_from, low_until). */
static int64_t s_rail_low_from_us;
static int64_t s_rail_low_until_us;
static float s_rail_low_v;
/* MAX-245: monitor latches an over-current from this time on. */
static int64_t s_rail_oc_from_us;
static unsigned s_oc_clear_calls;
static bool s_oc_cleared_before_gate_read;
static unsigned s_rail_reads;

typedef struct {
    ad7606_acquisition_test_log_event_t event;
    uint64_t sample_index;
    size_t channel;
    uint32_t count;
    const char *state;
    int64_t wall_time_us;
    int64_t pushed_wall_time_us;
    uint64_t pushed_sample_index;
    int64_t pushed_timestamp_us;
} test_log_record_t;

static test_log_record_t s_log_records[TEST_LOG_RECORD_CAPACITY];
static size_t s_log_record_count;
static uint64_t s_log_suppressed_count;
static uint64_t s_log_coalesced_count;
static uint64_t s_log_coalesced_first_sample;
static uint64_t s_log_coalesced_last_sample;
static uint64_t s_log_coalesced_samples[8];
static uint64_t s_log_sink_unavailable_count;
static uint32_t s_fake_diag_log_cost_us;
static uint32_t s_fake_serial_log_cost_us;
static bool s_fake_log_sink_available;
static bool s_log_allocation_baseline_set;
static uint32_t s_log_allocation_baseline;

static void reset_world(void)
{
    s_script_len = 0U;
    s_script_pos = 0U;
    s_now_us = 50000000LL;
    s_esp_timer_get_time_calls = 0U;
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
    s_last_push_sample_index = 0U;
    s_last_push_wall_time_us = 0;
    s_buffer_source = SEISMIC_SOURCE_TEST;
    s_buffer_rate = 12345U;
    s_ad7606_rate = 0U;
    s_running_seen_during_push = true;
    s_beats = 0U;
    s_notify_gives = 0U;
    s_notify_from_isr = 0U;
    s_busy_isr_calls = 0U;
    s_stop_isr_suppressions = 0U;
    s_task_blocking_calls = 0U;
    s_fake_serial_valid_after_conversion = 0U;
    s_fake_serial_clocklike_from_conversion = 0U;
    s_fake_serial_fixed_from_conversion = 0U;
    s_pending_notifications = 0U;
    s_synthetic_log_delay = false;
    s_worker_iteration_start_us = 0;
    s_worker_iteration_active = false;
    s_worker_iteration_published = false;
    s_worker_iteration_sample_index = 0U;
    s_last_synthetic_notification_batch = 0U;
    s_max_synthetic_notification_batch = 0U;
    s_fake_pcnt_count = 0U;
    s_fake_busy_level = 0;
    s_fake_busy_intr_type = GPIO_INTR_DISABLE;
    s_last_busy_intr_edge = GPIO_INTR_DISABLE;
    s_fake_busy_isr = NULL;
    s_fake_busy_isr_args = NULL;
    s_fake_gpio_intr_enabled = false;
    s_inject_busy_isr_on_stop = false;
    s_fake_pcnt_enabled = false;
    s_fake_pcnt_started = false;
    s_fake_ledc_timer_configured = false;
    s_fake_ledc_channel_configured = false;
    s_fake_ledc_frequency = 0U;
    s_last_ledc_frequency = 0U;
    s_last_ledc_duty = 0U;
    s_last_ledc_resolution = 0U;
    s_fake_watch_points = 0U;
    memset(s_push_timestamps, 0, sizeof(s_push_timestamps));
    memset(s_push_sample_indices, 0, sizeof(s_push_sample_indices));
    memset(s_worker_iteration_duration_us, 0, sizeof(s_worker_iteration_duration_us));
    memset(s_worker_iteration_duration_recorded, 0, sizeof(s_worker_iteration_duration_recorded));
    memset(s_push_wall_by_sample, 0, sizeof(s_push_wall_by_sample));
    memset(s_push_timestamp_by_sample, 0, sizeof(s_push_timestamp_by_sample));
    memset(s_push_recorded_by_sample, 0, sizeof(s_push_recorded_by_sample));
    memset(s_log_records, 0, sizeof(s_log_records));
    s_log_record_count = 0U;
    s_log_suppressed_count = 0U;
    s_log_coalesced_count = 0U;
    s_log_coalesced_first_sample = 0U;
    s_log_coalesced_last_sample = 0U;
    memset(s_log_coalesced_samples, 0, sizeof(s_log_coalesced_samples));
    s_log_sink_unavailable_count = 0U;
    s_fake_diag_log_cost_us = 0U;
    s_fake_serial_log_cost_us = 0U;
    s_fake_log_sink_available = true;
    s_log_allocation_baseline_set = false;
    s_log_allocation_baseline = 0U;
    ad7606_acquisition_test_log_sink_set(NULL);
    s_driver_init_result = ESP_OK;
    s_fake_measured_tconv_ns = 4000U;
    s_gpio_install_result = ESP_OK;
    s_ledc_timer_result = ESP_OK;
    s_read_result = ESP_OK;
    s_rail_available = true;
    s_rail_valid = true;
    s_rail_v = 5.02f;
    s_rail_fresh = true;
    s_rail_low_from_us = INT64_MAX;
    s_rail_low_until_us = INT64_MAX;
    s_rail_low_v = 0.0f;
    s_rail_oc_from_us = INT64_MAX;
    s_oc_clear_calls = 0U;
    s_oc_cleared_before_gate_read = false;
    s_rail_reads = 0U;
}

static void script(event_type_t type, uint32_t count)
{
    assert(s_script_len < sizeof(s_script) / sizeof(s_script[0]));
    s_script[s_script_len] = (event_t) {
        .type = type,
        .count = count,
        .timestamp_adjustment_us = 0,
    };
    ++s_script_len;
}

static void script_with_lateness(uint32_t count, int64_t lateness_us)
{
    assert(s_script_len < sizeof(s_script) / sizeof(s_script[0]));
    s_script[s_script_len] = (event_t) {
        .type = EV_TICK,
        .count = count,
        /* Task delay changes wall time, never the sample-index timestamp. */
        .timestamp_adjustment_us = lateness_us,
    };
    ++s_script_len;
}

static void call_fake_busy_isr(void)
{
    assert(s_fake_busy_isr != NULL);
    const unsigned timer_calls = s_esp_timer_get_time_calls;
    const unsigned task_creates = s_task_creates;
    const unsigned driver_inits = s_driver_inits;
    const unsigned driver_deinits = s_driver_deinits;
    const unsigned reads = s_reads;
    const unsigned rail_reads = s_rail_reads;
    const unsigned task_notifications = s_notify_gives;
    const unsigned blocking_calls = s_task_blocking_calls;
    const unsigned beats = s_beats;
    const unsigned pushes = s_pushes;
    const size_t log_records = s_log_record_count;
    const uint32_t allocations = ad7606_acquisition_test_heap_caps_allocations();
    const int critical_depth = s_critical_depth;
    const seismic_source_t buffer_source = s_buffer_source;
    const uint32_t buffer_rate = s_buffer_rate;

    ++s_busy_isr_calls;
    s_fake_busy_isr(s_fake_busy_isr_args);

    /* The ISR may notify the worker; task-only work stays in the worker. */
    assert(s_esp_timer_get_time_calls == timer_calls);
    assert(s_task_creates == task_creates);
    assert(s_driver_inits == driver_inits);
    assert(s_driver_deinits == driver_deinits);
    assert(s_reads == reads);
    assert(s_rail_reads == rail_reads);
    assert(s_notify_gives == task_notifications);
    assert(s_task_blocking_calls == blocking_calls);
    assert(s_beats == beats);
    assert(s_pushes == pushes);
    assert(s_log_record_count == log_records);
    assert(ad7606_acquisition_test_heap_caps_allocations() == allocations);
    assert(s_critical_depth == critical_depth);
    assert(s_buffer_source == buffer_source);
    assert(s_buffer_rate == buffer_rate);
}

static void fake_busy_conversion(void)
{
    s_fake_busy_level = 1;
    if (s_fake_gpio_intr_enabled &&
        s_fake_busy_intr_type == GPIO_INTR_POSEDGE && s_fake_busy_isr != NULL) {
        call_fake_busy_isr();
    }
    s_fake_busy_level = 0;
    ++s_fake_pcnt_count;
    if (s_fake_gpio_intr_enabled &&
        s_fake_busy_intr_type == GPIO_INTR_NEGEDGE && s_fake_busy_isr != NULL) {
        call_fake_busy_isr();
    }
}

static void finish_fake_worker_iteration(void)
{
    if (
        s_worker_iteration_active &&
        s_worker_iteration_published &&
        s_worker_iteration_sample_index < TEST_EVENT_CAPACITY
    ) {
        const size_t index = (size_t)s_worker_iteration_sample_index;
        s_worker_iteration_duration_us[index] =
            (uint64_t)(s_now_us - s_worker_iteration_start_us);
        s_worker_iteration_duration_recorded[index] = true;
    }
    s_worker_iteration_active = false;
    s_worker_iteration_published = false;
}

static void begin_fake_worker_iteration(void)
{
    s_worker_iteration_start_us = s_now_us;
    s_worker_iteration_active = true;
    s_worker_iteration_published = false;
}

static bool fake_log_sink(
    ad7606_acquisition_test_log_event_t event,
    uint64_t sample_index,
    size_t channel,
    uint32_t count,
    const char *state
)
{
    if (event == AD7606_ACQUISITION_TEST_LOG_SERIAL_SUPPRESSED) {
        ++s_log_suppressed_count;
    }
    else if (event == AD7606_ACQUISITION_TEST_LOG_SERIAL_COALESCED) {
        assert(s_log_coalesced_count <
            sizeof(s_log_coalesced_samples) / sizeof(s_log_coalesced_samples[0]));
        if (s_log_coalesced_count == 0U) {
            s_log_coalesced_first_sample = sample_index;
        }
        s_log_coalesced_last_sample = sample_index;
        s_log_coalesced_samples[s_log_coalesced_count] = sample_index;
        ++s_log_coalesced_count;
    }
    else {
        assert(s_log_record_count < TEST_LOG_RECORD_CAPACITY);
        s_log_records[s_log_record_count++] = (test_log_record_t) {
            .event = event,
            .sample_index = sample_index,
            .channel = channel,
            .count = count,
            .state = state,
            .wall_time_us = s_now_us,
            .pushed_wall_time_us = s_last_push_wall_time_us,
            .pushed_sample_index = s_last_push_sample_index,
            .pushed_timestamp_us = s_last_push_ts,
        };

        const uint32_t allocations =
            ad7606_acquisition_test_heap_caps_allocations();
        if (!s_log_allocation_baseline_set) {
            s_log_allocation_baseline = allocations;
            s_log_allocation_baseline_set = true;
        }
        else {
            assert(allocations == s_log_allocation_baseline);
        }

        if (!s_fake_log_sink_available) {
            ++s_log_sink_unavailable_count;
            return false;
        }

        const uint32_t cost_us =
            event == AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL
                ? s_fake_diag_log_cost_us
                : s_fake_serial_log_cost_us;
        if (cost_us != 0U) {
            s_synthetic_log_delay = true;
            for (uint32_t conversion = 0U;
                 conversion < cost_us / (uint32_t)PERIOD_US;
                 ++conversion) {
                fake_busy_conversion();
            }
            s_synthetic_log_delay = false;
            s_now_us += cost_us;
        }
    }
    return s_fake_log_sink_available;
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
    ++s_task_blocking_calls;
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
    if (s_stop_waiting) {
        /* The worker (priority 7) outranks every caller of stop(): the
         * notification preempts the caller and the worker runs at once,
         * seeing whatever s_running holds at this instant. */
        s_stop_waiting = false;
        longjmp(s_stop_return, 1);
    }
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher_priority_task_woken)
{
    assert(task == &s_task_token);
    ++s_notify_from_isr;
    if (s_synthetic_log_delay) {
        ++s_pending_notifications;
    }
    if (higher_priority_task_woken != NULL) {
        *higher_priority_task_woken = pdFALSE;
    }
}

void vTaskDelay(TickType_t ticks)
{
    ++s_task_blocking_calls;
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
    ++s_task_blocking_calls;
    assert(clear_on_exit == pdTRUE);
    assert(ticks_to_wait == portMAX_DELAY);
    assert(s_in_worker);
    finish_fake_worker_iteration();
    if (s_pending_notifications != 0U) {
        const uint32_t notifications = s_pending_notifications;
        s_pending_notifications = 0U;
        s_last_synthetic_notification_batch = notifications;
        if (notifications > s_max_synthetic_notification_batch) {
            s_max_synthetic_notification_batch = notifications;
        }
        begin_fake_worker_iteration();
        return notifications;
    }
    assert(s_script_pos < s_script_len); /* every test ends with STOP or self-stop */
    const event_t event = s_script[s_script_pos++];
    if (event.type == EV_TICK) {
        for (uint32_t conversion = 0U; conversion < event.count; ++conversion) {
            fake_busy_conversion();
        }
        s_now_us += PERIOD_US * (int64_t)event.count + event.timestamp_adjustment_us;
        begin_fake_worker_iteration();
        return event.count;
    }
    if (setjmp(s_stop_return) == 0) {
        s_stop_waiting = true;
        ad7606_acquisition_stop();
        assert(false); /* stop() must wait for the worker, not return */
    }
    return 1U;
}

/* ---- ESP timer / LEDC / PCNT / GPIO ---- */

int64_t esp_timer_get_time(void)
{
    ++s_esp_timer_get_time_calls;
    return s_now_us;
}

void esp_rom_delay_us(uint32_t us) { s_now_us += us; }

int gpio_get_level(gpio_num_t gpio_num)
{
    return gpio_num == 42 ? 0 : s_fake_busy_level;
}

esp_err_t gpio_install_isr_service(int flags)
{
    s_fake_gpio_install_flags = flags;
    return s_gpio_install_result;
}

esp_err_t board_gpio_isr_service_install(void)
{
    if (s_fake_isr_service_ready) {
        return ESP_OK;
    }
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_fake_isr_service_ready = true;
        return ESP_OK;
    }
    return err;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    assert(gpio_num == 47);
    s_fake_busy_intr_type = intr_type;
    if (intr_type != GPIO_INTR_DISABLE) {
        s_last_busy_intr_edge = intr_type;
    }
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr, void *args)
{
    assert(gpio_num == 47 && isr != NULL);
    s_fake_busy_isr = isr;
    s_fake_busy_isr_args = args;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    assert(gpio_num == 47);
    s_fake_busy_isr = NULL;
    s_fake_busy_isr_args = NULL;
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num)
{
    assert(gpio_num == 47);
    s_fake_gpio_intr_enabled = true;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    assert(gpio_num == 47);
    if (s_inject_busy_isr_on_stop) {
        s_inject_busy_isr_on_stop = false;
        assert(s_in_worker); /* task handle is still live during hardware cleanup */
        const unsigned notifications_before = s_notify_from_isr;
        call_fake_busy_isr();
        assert(s_notify_from_isr == notifications_before);
        ++s_stop_isr_suppressions;
    }
    s_fake_gpio_intr_enabled = false;
    return ESP_OK;
}

uint32_t ledc_find_suitable_duty_resolution(uint32_t source_hz, uint32_t timer_hz)
{
    uint32_t bits = 14U;
    while (bits > 0U && ((uint64_t)timer_hz << bits) > source_hz) {
        --bits;
    }
    return bits;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *config)
{
    if (config->deconfigure) {
        s_fake_ledc_timer_configured = false;
        s_fake_ledc_frequency = 0U;
        return ESP_OK;
    }
    if (s_ledc_timer_result != ESP_OK) {
        return s_ledc_timer_result;
    }
    assert(config->clk_cfg == LEDC_USE_XTAL_CLK);
    s_fake_ledc_timer_configured = true;
    s_fake_ledc_frequency = config->freq_hz;
    s_last_ledc_frequency = config->freq_hz;
    s_last_ledc_resolution = (uint32_t)config->duty_resolution;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    assert(s_fake_ledc_timer_configured);
    assert(config->gpio_num == 42);
    s_fake_ledc_channel_configured = true;
    s_last_ledc_duty = config->duty;
    return ESP_OK;
}

uint32_t ledc_get_freq(ledc_mode_t mode, ledc_timer_t timer)
{
    assert(mode == LEDC_LOW_SPEED_MODE && timer == LEDC_TIMER_0);
    return s_fake_ledc_frequency;
}

esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t channel, uint32_t idle_level)
{
    assert(mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0 && idle_level == 0U);
    s_fake_ledc_channel_configured = false;
    return ESP_OK;
}

esp_err_t ledc_timer_pause(ledc_mode_t mode, ledc_timer_t timer)
{
    assert(mode == LEDC_LOW_SPEED_MODE && timer == LEDC_TIMER_0);
    return ESP_OK;
}

struct test_pcnt_unit { int unused; };
struct test_pcnt_channel { int unused; };
static struct test_pcnt_unit s_fake_pcnt_unit;
static struct test_pcnt_channel s_fake_pcnt_channel;

esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config, pcnt_unit_handle_t *ret_unit)
{
    assert(config->low_limit < 0 && config->high_limit > 0);
    assert(config->flags.accum_count == 1U);
    *ret_unit = &s_fake_pcnt_unit;
    return ESP_OK;
}

esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit && !s_fake_pcnt_enabled);
    return ESP_OK;
}

esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int watch_point)
{
    assert(unit == &s_fake_pcnt_unit && (watch_point == 16384 || watch_point == -16384));
    ++s_fake_watch_points;
    return ESP_OK;
}

esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config,
    pcnt_channel_handle_t *ret_channel)
{
    assert(unit == &s_fake_pcnt_unit && config->edge_gpio_num == 47);
    *ret_channel = &s_fake_pcnt_channel;
    return ESP_OK;
}

esp_err_t pcnt_del_channel(pcnt_channel_handle_t channel)
{
    assert(channel == &s_fake_pcnt_channel);
    return ESP_OK;
}

esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel,
    pcnt_channel_edge_action_t pos_act, pcnt_channel_edge_action_t neg_act)
{
    assert(channel == &s_fake_pcnt_channel);
    assert(pos_act == PCNT_CHANNEL_EDGE_ACTION_HOLD);
    assert(neg_act == PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    return ESP_OK;
}

esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t channel,
    pcnt_channel_level_action_t high_act, pcnt_channel_level_action_t low_act)
{
    assert(channel == &s_fake_pcnt_channel);
    (void)high_act;
    (void)low_act;
    return ESP_OK;
}

esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit);
    s_fake_pcnt_count = 0U;
    return ESP_OK;
}

esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit);
    s_fake_pcnt_enabled = true;
    return ESP_OK;
}

esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit && !s_fake_pcnt_started);
    s_fake_pcnt_enabled = false;
    return ESP_OK;
}

esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit && s_fake_pcnt_enabled);
    s_fake_pcnt_started = true;
    return ESP_OK;
}

esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit)
{
    assert(unit == &s_fake_pcnt_unit && s_fake_pcnt_enabled);
    s_fake_pcnt_started = false;
    return ESP_OK;
}

esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *value)
{
    assert(unit == &s_fake_pcnt_unit && value != NULL);
    *value = (int32_t)s_fake_pcnt_count;
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
    ++s_fake_pcnt_count; /* Driver self-test confirms BUSY reaches PCNT. */
    return ESP_OK;
}

esp_err_t ad7606_driver_get_measured_tconv_ns(uint32_t *tconv_ns)
{
    assert(tconv_ns != NULL);
    *tconv_ns = s_fake_measured_tconv_ns;
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

esp_err_t ad7606_driver_read_result(ad7606_sample_t *sample)
{
    ++s_reads;
    if (!s_driver_live) {
        ++s_reads_after_deinit;
    }
    if (s_read_result != ESP_OK) {
        return s_read_result;
    }
    const unsigned completed_conversion = s_fake_pcnt_count > 0U
        ? (unsigned)s_fake_pcnt_count
        : 0U;
    sample->timestamp_us = 0;
    for (unsigned c = 0U; c < AD7606_CHANNEL_COUNT; ++c) {
        sample->raw[c] = raw_for(completed_conversion, c);
    }
    return ESP_OK;
}

esp_err_t ad7606_driver_read(ad7606_sample_t *sample)
{
    return ad7606_driver_read_result(sample);
}

esp_err_t ad7606_driver_get_last_serial_observation(ad7606_serial_observation_t *observation)
{
    memset(observation, 0, sizeof(*observation));
    if (
        s_fake_serial_valid_after_conversion != 0U &&
        s_fake_pcnt_count < s_fake_serial_valid_after_conversion
    ) {
        return ESP_FAIL;
    }
    observation->conversion_complete = true;
    observation->spi_transaction_ok = true;
    if (
        s_fake_serial_clocklike_from_conversion != 0U &&
        s_fake_pcnt_count >= s_fake_serial_clocklike_from_conversion
    ) {
        memset(observation->rx, 0xAA, sizeof(observation->rx));
        return ESP_OK;
    }
    if (
        s_fake_serial_fixed_from_conversion != 0U &&
        s_fake_pcnt_count >= s_fake_serial_fixed_from_conversion
    ) {
        static const uint8_t varied_frame[AD7606_SERIAL_RX_BYTES] = {
            0x12U, 0x34U, 0x23U, 0x45U,
            0x34U, 0x56U, 0x45U, 0x67U,
        };
        memcpy(observation->rx, varied_frame, sizeof(varied_frame));
        return ESP_OK;
    }
    for (unsigned c = 0U; c < AD7606_CHANNEL_COUNT; ++c) {
        const uint16_t word = (uint16_t)raw_for(
            s_fake_pcnt_count > 0U ? (unsigned)s_fake_pcnt_count : 0U,
            c
        );
        observation->rx[2U * c] = (uint8_t)(word >> 8);
        observation->rx[2U * c + 1U] = (uint8_t)(word & 0xFFU);
    }
    return ESP_OK;
}

float ad7606_raw_to_uv(int16_t raw)
{
    return (float)raw * 152.587890625f; /* 5 V / 32768 */
}

bool ad7606_raw_to_uv_for_config(
    int16_t raw,
    uint8_t oversampling_ratio,
    uint32_t full_scale_mv,
    float *microvolts
)
{
    (void)oversampling_ratio;
    if (microvolts == NULL) return false;
    *microvolts = (float)raw * ((float)full_scale_mv * 1000.0f / 32768.0f);
    return true;
}

void ad7606_driver_presence_use_external_busy_isr(bool enabled)
{
    (void)enabled;
}

void ad7606_driver_presence_note_busy_pulse(void) {}

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
    if (s_now_us >= s_rail_oc_from_us) {
        rail->overcurrent_level = 2;
        rail->overcurrent_latched = true;
        rail->overcurrent_latched_current_a = 0.42f;
    }
    if (s_rail_reads == 0U) {
        s_oc_cleared_before_gate_read = s_oc_clear_calls == 1U;
    }
    ++s_rail_reads;
    return ESP_OK;
}

esp_err_t ina3221_monitor_clear_overcurrent(size_t channel_index)
{
    assert(channel_index == INA3221_MONITOR_RAIL_AD7606_5V);
    assert(!s_in_worker); /* cleared by the explicit start, not by the worker */
    ++s_oc_clear_calls;
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

esp_err_t seismic_buffer_push_indexed(
    int64_t timestamp_us,
    uint64_t source_sample_index,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    if (s_pushes < sizeof(s_push_timestamps) / sizeof(s_push_timestamps[0])) {
        s_push_timestamps[s_pushes] = timestamp_us;
        s_push_sample_indices[s_pushes] = source_sample_index;
    }
    ++s_pushes;
    s_last_push_ts = timestamp_us;
    s_last_push_sample_index = source_sample_index;
    s_last_push_wall_time_us = s_now_us;
    if (source_sample_index < TEST_EVENT_CAPACITY) {
        const size_t index = (size_t)source_sample_index;
        s_push_wall_by_sample[index] = s_now_us;
        s_push_timestamp_by_sample[index] = timestamp_us;
        s_push_recorded_by_sample[index] = true;
    }
    if (s_worker_iteration_active) {
        s_worker_iteration_sample_index = source_sample_index;
        s_worker_iteration_published = true;
    }
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
    assert(!s_driver_live);
    assert(!s_fake_pcnt_enabled);
    assert(!s_fake_pcnt_started);
    assert(!s_fake_ledc_timer_configured);
    assert(!s_fake_ledc_channel_configured);
    assert(!s_fake_gpio_intr_enabled);
    assert(s_buffer_source == SEISMIC_SOURCE_NONE);
    assert(!status_now().running);
    assert(!s_sem_live);
}

/* ---- tests ---- */

static void test_run_samples_then_stop_releases_everything(void)
{
    reset_world();
    s_inject_busy_isr_on_stop = true;
    const int64_t sample_t0 = s_now_us;
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
    assert(s_last_ledc_frequency == 500U);
    assert(s_last_ledc_resolution == 14U);
    assert(s_last_ledc_duty == 18U);
    assert(s_last_busy_intr_edge == GPIO_INTR_NEGEDGE);
    assert(s_fake_gpio_install_flags == ESP_INTR_FLAG_IRAM);
    assert(s_fake_watch_points == 2U);
    assert(s_notify_from_isr == 3U);
    assert(s_busy_isr_calls == 4U); /* three edges plus a STOP-time probe */
    assert(s_stop_isr_suppressions == 1U);
    assert(s_esp_timer_get_time_calls == 2U); /* start gate + LEDC t0, never per sample */
    assert(s_ad7606_rate == 500U);
    assert(s_buffer_rate == 0U); /* detached again on stop */

    /* Three periods -> three conversions -> three pushes of channels 1..4. */
    assert(s_reads == 3U);
    assert(s_pushes == 3U);
    assert(s_running_seen_during_push);
    assert(s_push_timestamps[0] == sample_t0);
    assert(s_push_timestamps[1] == sample_t0 + PERIOD_US);
    assert(s_last_push_ts == sample_t0 + 2 * PERIOD_US);
    for (unsigned c = 0U; c < SEISMIC_CHANNEL_COUNT; ++c) {
        assert(fabsf(s_last_push[c] - ad7606_raw_to_uv(raw_for(3U, c))) < 1e-3f);
    }
    assert(s_beats >= 3U);

    /* STOP: hardware clocks and counters stopped, driver deinit (-> Hi-Z safe state),
     * buffer detached, status not running. */
    assert(s_driver_deinits == 1U);
    assert(s_reads_after_deinit == 0U);
    assert_fully_released();

    const ad7606_acquisition_status_t status = status_now();
    assert(status.samples == 3U);
    assert(status.read_errors == 0U);
    /* The STOP notification wakes the worker but is not a timer period:
     * it is neither counted nor converted (s_running is cleared before
     * the worker is notified). */
    assert(status.timer_ticks == 3U);
    assert(status.conversion_attempts == 3U);
    assert(status.collapsed_periods == 0U);
    assert(status.power_gate == AD7606_POWER_GATE_OK);
    assert(!status.power_fault_latched);
    assert(status.os_ratio_configured == 1U);
    assert(status.os_ratio_measured == 1U);
    assert(!status.os_mismatch);

    assert(s_fake_busy_isr == NULL); /* no BUSY ISR survives stop */
}


static void test_safe_os_mismatch_runs_and_is_reported(void)
{
    reset_world();
    s_fake_measured_tconv_ns = 17000U; /* OS4 while Kconfig remains OS1. */
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.os_ratio_configured == 1U);
    assert(status.os_ratio_measured == 4U);
    assert(status.os_mismatch);
    assert_fully_released();
}


static void test_unknown_os_measurement_refuses_start(void)
{
    reset_world();
    s_fake_measured_tconv_ns = 5000U; /* Outside every calibrated interval. */
    assert(ad7606_acquisition_start() == ESP_ERR_INVALID_RESPONSE);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.os_ratio_configured == 1U);
    assert(status.os_ratio_measured == 0U);
    assert(status.os_mismatch);
    assert_fully_released();
}

static void test_collapsed_periods_are_one_conversion(void)
{
    reset_world();
    const int64_t sample_t0 = s_now_us;
    script_with_lateness(4U, 17); /* four conversion edges coalesce before task service */
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(s_notify_from_isr == 4U);
    assert(s_busy_isr_calls == 4U);
    assert(s_reads == 1U);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.timer_ticks == 4U);
    assert(status.conversion_attempts == 1U);
    assert(status.collapsed_periods == 3U);
    assert(status.jitter.count == 0U);
    assert(s_push_timestamps[0] == sample_t0 + 3 * PERIOD_US);
    assert(s_push_sample_indices[0] == 3U);
    /* Collapsed periods are PCNT-observed conversions without a pushed frame. */
    assert(status.read_errors == 0U);
    assert(status.consecutive_failures == 0U);
}

static void test_successful_read_jitter_statistics(void)
{
    reset_world();
    const int64_t sample_t0 = s_now_us;
    const int64_t lateness_us[] = { 0, 1, 3, 17, 1000, 40000 };
    for (size_t i = 0U; i < sizeof(lateness_us) / sizeof(lateness_us[0]); ++i) {
        script_with_lateness(1U, lateness_us[i]);
    }
    script(EV_STOP, 0U);

    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.jitter.count == 0U); /* no per-sample esp_timer timestamping */
    assert(status.jitter.period_us == (uint64_t)PERIOD_US);
    assert(s_pushes == 6U);
    for (size_t sample = 0U; sample < 6U; ++sample) {
        assert(s_push_timestamps[sample] == sample_t0 + (int64_t)sample * PERIOD_US);
    }
}

static void test_jitter_bucket_boundaries_and_open_ended_bucket(void)
{
    reset_world();
    const int64_t sample_t0 = s_now_us;
    script_with_lateness(1U, 1);
    script_with_lateness(1U, 2);
    script_with_lateness(1U, 3);
    script_with_lateness(1U, 4);
    script_with_lateness(1U, INT64_C(1) << 15);
    script_with_lateness(1U, INT64_C(1) << 40);
    script(EV_STOP, 0U);

    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.jitter.count == 0U);
    assert(s_pushes == 6U);
    for (size_t sample = 0U; sample < 6U; ++sample) {
        assert(s_push_timestamps[sample] == sample_t0 + (int64_t)sample * PERIOD_US);
    }
}

static void test_negative_jitter_is_clamped_and_counted_early(void)
{
    reset_world();
    const int64_t sample_t0 = s_now_us;
    script_with_lateness(1U, -5);
    script(EV_STOP, 0U);

    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.jitter.count == 0U);
    assert(s_push_timestamps[0] == sample_t0);
}

static void test_jitter_counter_saturation(void)
{
    assert(ad7606_acquisition_saturating_add_u32(UINT32_MAX - 1U, 1U) == UINT32_MAX);
    assert(ad7606_acquisition_saturating_add_u32(UINT32_MAX, 1U) == UINT32_MAX);
}

static void test_low_rail_refuses_start_without_touching_the_adc(void)
{
    reset_world();
    s_rail_v = 4.60f; /* fresh reading below the 4.75 V start minimum */
    assert(ad7606_acquisition_start() == ESP_ERR_INVALID_STATE);
    assert(s_task_creates == 0U);
    assert(s_driver_inits == 0U);
    assert(!s_fake_ledc_channel_configured);
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
    /* The rail collapses 1.5 s into the run. PCNT gates checks to 1 Hz;
     * two fresh low readings confirm the loss. */
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
    /* One 1 Hz rail check lands in a short dip. */
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
    /* Each failed, PCNT-indexed output is now an explicit acquisition gap. */
    assert(status.collapsed_periods == 2U);
    assert(status.timer_ticks == 2U);
    assert(s_beats >= 2U); /* still alive for the watchdog */
    assert_fully_released();
}

static void test_startup_failures_clean_up(void)
{
    /* Driver init and LEDC timer failures report the error and nothing stays
     * configured or driven. ISR service installation is covered by its owner. */
    for (int which = 0; which < 2; ++which) {
        reset_world();
        const esp_err_t injected = which == 0 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
        if (which == 0) s_driver_init_result = injected;
        if (which == 1) s_ledc_timer_result = injected;
        assert(ad7606_acquisition_start() == injected);
        assert(s_driver_deinits == 1U);
        assert(s_reads == 0U);
        assert(s_script_pos == 0U);
        assert_fully_released();
    }
}

static void test_restart_resets_cadence_counters(void)
{
    reset_world();
    script_with_lateness(5U, 40000);
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t first_run = status_now();
    assert(first_run.collapsed_periods == 4U);
    assert(first_run.jitter.count == 0U);

    reset_world();
    script_with_lateness(1U, 3);
    script(EV_STOP, 0U);
    assert(ad7606_acquisition_start() == ESP_OK);
    const ad7606_acquisition_status_t status = status_now();
    assert(status.timer_ticks == 1U);
    assert(status.collapsed_periods == 0U);
    assert(status.samples == 1U);
    assert(status.jitter.count == 0U);
    assert(status.jitter.period_us == (uint64_t)PERIOD_US);
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

static void test_overcurrent_stops_and_latches(void)
{
    reset_world();
    const int64_t t0 = s_now_us;
    s_rail_oc_from_us = t0 + 1500000LL;
    for (int i = 0; i < 40; ++i) {
        script(EV_TICK, 100U); /* 0.2 s per wake */
    }
    /* No STOP: the worker must stop by itself at the next rail check. */
    assert(ad7606_acquisition_start() == ESP_OK);
    /* The explicit start cleared the previous latch before deciding. */
    assert(s_oc_clear_calls == 1U);
    assert(s_oc_cleared_before_gate_read);

    const ad7606_acquisition_status_t status = status_now();
    assert(status.power_fault_latched);
    assert(status.power_gate == AD7606_POWER_GATE_OVERCURRENT);
    assert(fabsf(status.power_fault_current_a - 0.42f) < 1e-6f);
    assert(s_driver_deinits == 1U);
    assert(s_reads_after_deinit == 0U);
    assert_fully_released();
    /* Checks at ~0.2, 1.2 (clean) and 2.2 s (latched) -> stopped there. */
    assert(s_now_us - t0 >= 2000000LL && s_now_us - t0 <= 2400000LL);
    assert(s_script_pos < s_script_len);
}

typedef struct
{
    uint32_t count;
    int16_t min_raw;
    int16_t max_raw;
    int64_t sum_raw;
    uint64_t sum_sq_raw;
    uint32_t saturation_count;

} diag_reference_t;

static diag_reference_t s_diag_reference[AD7606_CHANNEL_COUNT];

static void diag_reference_reset(void)
{
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        s_diag_reference[channel] = (diag_reference_t) {
            .min_raw = INT16_MAX,
            .max_raw = INT16_MIN,
        };
    }
}

static void diag_reset_both(void)
{
    ad7606_acquisition_test_diag_reset();
    diag_reference_reset();
}

static void diag_reference_record(diag_reference_t *diag, int16_t raw)
{
    const int64_t widened = (int64_t)raw;
    if (raw < diag->min_raw) diag->min_raw = raw;
    if (raw > diag->max_raw) diag->max_raw = raw;
    diag->count += 1U;
    diag->sum_raw += widened;
    diag->sum_sq_raw += (uint64_t)(widened * widened);
    if (raw == INT16_MIN || raw == INT16_MAX) diag->saturation_count += 1U;
}

static void diag_push_values(
    int16_t ch0,
    int16_t ch1,
    int16_t ch2,
    int16_t ch3
)
{
    const int16_t raw[AD7606_CHANNEL_COUNT] = { ch0, ch1, ch2, ch3 };
    ad7606_sample_t sample = { 0 };
    memcpy(sample.raw, raw, sizeof(raw));
    ad7606_acquisition_test_diag_record(&sample);
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        diag_reference_record(&s_diag_reference[channel], raw[channel]);
    }
}

static const char *diag_reference_state(const diag_reference_t *diag)
{
    if (diag->saturation_count != 0U) return "CLIP";
    if (diag->count != 0U && diag->min_raw == diag->max_raw) return "FLAT";
    return "VAR";
}

static void diag_reference_metrics(const diag_reference_t *diag, double out[4])
{
    const double count = (double)diag->count;
    const double mean = (double)diag->sum_raw / count;
    double variance = (double)diag->sum_sq_raw / count - mean * mean;
    if (variance < 0.0) variance = 0.0;
    const double max_delta = fabs((double)diag->max_raw - mean);
    const double min_delta = fabs((double)diag->min_raw - mean);
    out[0] = mean;
    out[1] = sqrt(variance);
    out[2] = (double)diag->max_raw - (double)diag->min_raw;
    out[3] = max_delta > min_delta ? max_delta : min_delta;
}

static void diag_compare_with_reference(void)
{
    ad7606_diag_channel_t actual[AD7606_CHANNEL_COUNT];
    ad7606_acquisition_test_diag_snapshot(actual);
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        const diag_reference_t *expected = &s_diag_reference[channel];
        const ad7606_diag_channel_t *observed = &actual[channel];
        assert(observed->count == expected->count);
        assert(observed->min_raw == expected->min_raw);
        assert(observed->max_raw == expected->max_raw);
        assert(observed->sum_raw == expected->sum_raw);
        assert(observed->sum_sq_raw == expected->sum_sq_raw);
        assert(observed->saturation_count == expected->saturation_count);
        assert(strcmp(
            ad7606_acquisition_test_diag_state(channel),
            diag_reference_state(expected)
        ) == 0);

        if (expected->count != 0U) {
            ad7606_diag_metrics_t metrics;
            double reference[4];
            diag_reference_metrics(expected, reference);
            assert(ad7606_acquisition_test_diag_metrics(channel, &metrics));
            const double actual_metrics[4] = {
                metrics.mean_raw,
                metrics.rms_raw,
                metrics.p2p_raw,
                metrics.peak_raw,
            };
            for (size_t metric = 0U; metric < 4U; ++metric) {
                const double tolerance =
                    1e-10 * fmax(1.0, fabs(reference[metric]));
                assert(fabs(actual_metrics[metric] - reference[metric]) <= tolerance);
            }
        }
        else {
            ad7606_diag_metrics_t unused;
            assert(!ad7606_acquisition_test_diag_metrics(channel, &unused));
        }
    }
}

static void test_diag_basic_values_and_channel_independence(void)
{
    diag_reset_both();
    for (size_t sample = 0U; sample < 8U; ++sample) {
        diag_push_values(0, 0, 0, 0);
    }
    diag_compare_with_reference();

    diag_reset_both();
    diag_push_values(1, -1, 1, -1);
    diag_push_values(-1, 1, -1, 1);
    diag_compare_with_reference();

    diag_reset_both();
    diag_push_values(INT16_MIN, INT16_MAX, -123, 456);
    diag_push_values(INT16_MAX, INT16_MIN, 789, -321);
    diag_compare_with_reference();

    diag_reset_both();
    for (int32_t value = -100; value <= 100; ++value) {
        diag_push_values(
            (int16_t)value,
            (int16_t)-value,
            (int16_t)(value * 3),
            (int16_t)(value * 7)
        );
    }
    diag_compare_with_reference();

    diag_reset_both();
    for (int32_t value = -32; value < 32; ++value) {
        diag_push_values(
            (int16_t)value,
            (int16_t)(value * 2),
            (int16_t)(value * 3),
            (int16_t)(value * 4)
        );
    }
    diag_push_values(1, 2, INT16_MIN, 4);
    diag_compare_with_reference();
    assert(strcmp(ad7606_acquisition_test_diag_state(2U), "CLIP") == 0);
    assert(strcmp(ad7606_acquisition_test_diag_state(0U), "VAR") == 0);
}

static void test_diag_supported_rate_windows_and_overflow_bound(void)
{
    const uint32_t rates[] = { 500U, 5000U, 10000U, 20000U, 40000U };
    const uint64_t worst_window_sum =
        (uint64_t)AD7606_DIAG_MAX_WINDOW_SAMPLES * (UINT64_C(1) << 30U);
    assert(worst_window_sum == UINT64_C(42949672960000));
    assert(UINT64_MAX / worst_window_sum > 429000U);

    for (size_t rate_index = 0U; rate_index < sizeof(rates) / sizeof(rates[0]); ++rate_index) {
        const uint32_t rate = rates[rate_index];
        diag_reset_both();
        diag_push_values(INT16_MIN, INT16_MAX, 7, -1);
        diag_compare_with_reference();

        diag_reset_both();
        for (uint32_t sample = 0U; sample < rate; ++sample) {
            const bool odd = (sample & 1U) != 0U;
            diag_push_values(
                odd ? INT16_MAX : INT16_MIN,
                odd ? INT16_MIN : INT16_MAX,
                odd ? INT16_MAX : INT16_MIN,
                odd ? INT16_MIN : INT16_MAX
            );
        }
        diag_compare_with_reference();
        ad7606_diag_channel_t actual[AD7606_CHANNEL_COUNT];
        ad7606_acquisition_test_diag_snapshot(actual);
        for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
            assert(actual[channel].count == rate);
            assert(actual[channel].sum_sq_raw ==
                ((uint64_t)rate / 2U) *
                    ((UINT64_C(1) << 30U) + UINT64_C(32767) * UINT64_C(32767)));
        }
    }

    diag_reset_both();
    for (uint32_t sample = 0U; sample < AD7606_DIAG_MAX_WINDOW_SAMPLES; ++sample) {
        diag_push_values(INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN);
    }
    diag_compare_with_reference();
    ad7606_diag_channel_t actual[AD7606_CHANNEL_COUNT];
    ad7606_acquisition_test_diag_snapshot(actual);
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        assert(actual[channel].sum_sq_raw == worst_window_sum);
    }

    diag_reset_both();
    for (uint32_t sample = 0U; sample < AD7606_DIAG_MAX_WINDOW_SAMPLES; ++sample) {
        diag_push_values(INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX);
    }
    diag_compare_with_reference();
}

static void test_diag_report_reset_boundary(void)
{
    const uint64_t rate = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;
    diag_reset_both();
    for (uint64_t sample = 0U; sample < rate - 1U; ++sample) {
        const int16_t value = (int16_t)(sample % 100U);
        diag_push_values(value, (int16_t)-value, value, (int16_t)-value);
        ad7606_acquisition_test_diag_finish_sample(sample);
    }
    diag_compare_with_reference();

    diag_push_values(17, -18, 19, -20);
    diag_compare_with_reference(); /* N records exist immediately before finish. */
    ad7606_acquisition_test_diag_finish_sample(rate - 1U);
    diag_reset_both();
    diag_compare_with_reference(); /* The production report/reset hook cleared N. */

    diag_push_values(21, -22, 23, -24);
    ad7606_acquisition_test_diag_finish_sample(rate);
    diag_compare_with_reference(); /* N+1 is the first sample of the next window. */
}

static void reset_log_capture(void)
{
    memset(s_log_records, 0, sizeof(s_log_records));
    s_log_record_count = 0U;
    s_log_suppressed_count = 0U;
    s_log_coalesced_count = 0U;
    s_log_coalesced_first_sample = 0U;
    s_log_coalesced_last_sample = 0U;
    memset(s_log_coalesced_samples, 0, sizeof(s_log_coalesced_samples));
    s_log_sink_unavailable_count = 0U;
    s_fake_diag_log_cost_us = 0U;
    s_fake_serial_log_cost_us = 0U;
    s_fake_log_sink_available = true;
    s_log_allocation_baseline_set = false;
    s_log_allocation_baseline = 0U;
    ad7606_acquisition_test_log_sink_set(fake_log_sink);
}

static void assert_diag_window_report(
    uint64_t sample_index,
    uint32_t count,
    const char *expected_states[AD7606_CHANNEL_COUNT]
)
{
    size_t channel_logs = 0U;
    for (size_t i = 0U; i < s_log_record_count; ++i) {
        const test_log_record_t *record = &s_log_records[i];
        if (record->event != AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL) {
            continue;
        }
        assert(record->sample_index == sample_index);
        assert(record->channel < AD7606_CHANNEL_COUNT);
        assert(record->count == count);
        assert(strcmp(record->state, expected_states[record->channel]) == 0);
        ++channel_logs;
    }
    assert(channel_logs == AD7606_CHANNEL_COUNT);
}

static void test_diag_window_report_semantics(void)
{
    const uint32_t rate = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;
    const char *varied_states[AD7606_CHANNEL_COUNT] = {
        "VAR", "VAR", "VAR", "VAR",
    };
    const char *flat_states[AD7606_CHANNEL_COUNT] = {
        "FLAT", "FLAT", "FLAT", "FLAT",
    };
    const char *clipped_states[AD7606_CHANNEL_COUNT] = {
        "CLIP", "VAR", "VAR", "VAR",
    };

    reset_log_capture();
    diag_reset_both();
    for (uint32_t sample = 0U; sample < rate; ++sample) {
        const int16_t value = (int16_t)((int32_t)(sample % 101U) - 50);
        diag_push_values(value, (int16_t)-value, (int16_t)(value + 1),
            (int16_t)-(value + 1));
    }
    diag_compare_with_reference();
    ad7606_acquisition_test_diag_finish_sample(rate - 1U);
    assert_diag_window_report(rate - 1U, rate, varied_states);

    ad7606_diag_channel_t actual[AD7606_CHANNEL_COUNT];
    ad7606_acquisition_test_diag_snapshot(actual);
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        assert(actual[channel].count == 0U);
    }

    /* The first sample after report/reset belongs to the next window. */
    diag_reference_reset();
    diag_push_values(21, -22, 23, -24);
    diag_compare_with_reference();
    assert(s_log_record_count == AD7606_CHANNEL_COUNT);

    reset_log_capture();
    diag_reset_both();
    for (uint32_t sample = 0U; sample < rate; ++sample) {
        diag_push_values(7, -7, 0, 1);
    }
    diag_compare_with_reference();
    ad7606_acquisition_test_diag_finish_sample(rate - 1U);
    assert_diag_window_report(rate - 1U, rate, flat_states);

    reset_log_capture();
    diag_reset_both();
    for (uint32_t sample = 0U; sample < rate; ++sample) {
        const int16_t value = (int16_t)((int32_t)(sample % 101U) - 50);
        diag_push_values(INT16_MIN, value, (int16_t)-value,
            (int16_t)(value * 2));
    }
    diag_compare_with_reference();
    ad7606_acquisition_test_diag_finish_sample(rate - 1U);
    assert_diag_window_report(rate - 1U, rate, clipped_states);

    ad7606_acquisition_test_log_sink_set(NULL);
}

static void append_sample_ticks(uint32_t samples)
{
    for (uint32_t sample = 0U; sample < samples; ++sample) {
        script(EV_TICK, 1U);
    }
}

static size_t log_record_count(ad7606_acquisition_test_log_event_t event)
{
    size_t count = 0U;
    for (size_t i = 0U; i < s_log_record_count; ++i) {
        if (s_log_records[i].event == event) {
            ++count;
        }
    }
    return count;
}

static const test_log_record_t *find_log_record(
    ad7606_acquisition_test_log_event_t event,
    uint64_t sample_index
)
{
    for (size_t i = 0U; i < s_log_record_count; ++i) {
        if (
            s_log_records[i].event == event &&
            s_log_records[i].sample_index == sample_index
        ) {
            return &s_log_records[i];
        }
    }
    assert(false);
    return NULL;
}

static uint64_t worker_iteration_us(uint64_t sample_index)
{
    assert(sample_index < TEST_EVENT_CAPACITY);
    assert(s_worker_iteration_duration_recorded[sample_index]);
    return s_worker_iteration_duration_us[sample_index];
}

static void test_diag_logging_virtual_time_characterization(void)
{
    const uint32_t rate = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;
    const uint32_t injected_cost_us = 2390U;
    reset_world();
    reset_log_capture();
    s_fake_diag_log_cost_us = injected_cost_us;
    s_fake_serial_fixed_from_conversion = 1U;
    append_sample_ticks(rate);
    script(EV_STOP, 0U);

    const uint32_t allocations_before =
        ad7606_acquisition_test_heap_caps_allocations();
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(ad7606_acquisition_test_heap_caps_allocations() ==
        allocations_before + 1U); /* only the serial quality-window buffer */

    const uint64_t ordinary_iteration_us = worker_iteration_us(10U);
    const uint64_t before_boundary_iteration_us = worker_iteration_us(rate - 2U);
    const uint64_t boundary_iteration_us = worker_iteration_us(rate - 1U);
    assert(ordinary_iteration_us == 0U);
    assert(before_boundary_iteration_us == 0U);
    /* CHARACTERIZATION: current Core-1 logging consumes the exact injected cost.
     * A MAX-366 deferral changes this baseline to zero boundary cost and no collapse. */
    assert(boundary_iteration_us == 4U * injected_cost_us);
    assert(boundary_iteration_us - ordinary_iteration_us == 9560U);
    assert(s_push_recorded_by_sample[10U]);
    assert(s_push_recorded_by_sample[11U]);
    assert(s_push_wall_by_sample[11U] - s_push_wall_by_sample[10U] == PERIOD_US);
    assert(s_push_recorded_by_sample[rate - 2U]);
    assert(s_push_recorded_by_sample[rate - 1U]);
    assert(s_push_wall_by_sample[rate - 1U] -
        s_push_wall_by_sample[rate - 2U] == PERIOD_US);
    assert(s_last_synthetic_notification_batch == 4U);
    assert(s_pushes == rate + 1U);
    assert(s_last_push_sample_index == rate + 3U);
    assert(s_script_pos == rate + 1U);

    const char *varied_states[AD7606_CHANNEL_COUNT] = {
        "VAR", "VAR", "VAR", "VAR",
    };
    assert_diag_window_report(rate - 1U, rate, varied_states);
    assert(log_record_count(AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL) ==
        AD7606_CHANNEL_COUNT); /* one report in this one-second window */
    const test_log_record_t *boundary_log = find_log_record(
        AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL,
        rate - 1U
    );
    assert(boundary_log->channel == 0U);
    assert(boundary_log->pushed_sample_index == rate - 1U);
    assert(boundary_log->pushed_timestamp_us ==
        s_push_timestamp_by_sample[0U] + (int64_t)(rate - 1U) * PERIOD_US);
    assert(boundary_log->wall_time_us == boundary_log->pushed_wall_time_us);
    assert(s_push_timestamp_by_sample[rate + 3U] ==
        s_push_timestamp_by_sample[0U] + (int64_t)(rate + 3U) * PERIOD_US);

    ad7606_diag_channel_t actual[AD7606_CHANNEL_COUNT];
    ad7606_acquisition_test_diag_snapshot(actual);
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        assert(actual[channel].count == 1U);
        assert(actual[channel].min_raw == raw_for((unsigned)(rate + 4U),
            (unsigned)channel));
        assert(actual[channel].max_raw == actual[channel].min_raw);
    }

    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    assert(status.collapsed_periods == 3U);
    assert(!status.running);
    printf(
        "MAX-366 virtual us: ordinary_iteration=%llu pre_boundary_iteration=%llu "
        "diag_boundary_iteration=%llu boundary_minus_steady=%llu "
        "notifications_coalesced=4 collapsed_conversions=3\n",
        (unsigned long long)ordinary_iteration_us,
        (unsigned long long)before_boundary_iteration_us,
        (unsigned long long)boundary_iteration_us,
        (unsigned long long)(boundary_iteration_us - ordinary_iteration_us)
    );
}

static void test_serial_diag_rate_limit_boundary(void)
{
    const uint32_t rate = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;
    const uint32_t injected_cost_us = 2390U;
    reset_world();
    reset_log_capture();
    s_fake_serial_clocklike_from_conversion = 2U;
    s_fake_serial_log_cost_us = injected_cost_us;
    append_sample_ticks(rate);
    script(EV_STOP, 0U);

    const uint32_t allocations_before =
        ad7606_acquisition_test_heap_caps_allocations();
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(ad7606_acquisition_test_heap_caps_allocations() ==
        allocations_before + 1U);

    assert(log_record_count(AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG) == 2U);
    const test_log_record_t *first_serial = find_log_record(
        AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG,
        0U
    );
    const test_log_record_t *boundary_serial = find_log_record(
        AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG,
        rate
    );
    assert(strcmp(first_serial->state, "NORMAL") == 0);
    assert(strcmp(boundary_serial->state, "SUSPICIOUS") == 0);
    assert(boundary_serial->pushed_sample_index == rate - 1U);
    assert(s_log_suppressed_count == rate - 1U);
    assert(s_log_coalesced_count == 3U);
    assert(s_log_coalesced_first_sample == 9U);
    assert(s_log_coalesced_samples[1] == rate - 1U);
    assert(s_log_coalesced_last_sample == rate);
    assert(!ad7606_acquisition_test_serial_diag_pending());
    assert(ad7606_acquisition_test_serial_diag_last_log_sample() == rate + 1U);
    assert(worker_iteration_us(0U) == injected_cost_us);
    assert(worker_iteration_us(rate - 1U) == 0U);
    assert(worker_iteration_us(rate) == injected_cost_us);
    assert(s_last_synthetic_notification_batch == 1U);

    const char *varied_states[AD7606_CHANNEL_COUNT] = {
        "VAR", "VAR", "VAR", "VAR",
    };
    assert_diag_window_report(rate - 1U, rate, varied_states);
    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    assert(status.serial_suspicious);
    assert(status.collapsed_periods == 0U);
    assert(!status.running);
    printf(
        "MAX-366 virtual us: serial_diag_boundary_iteration=%llu "
        "suppressed=%llu coalesced=%llu reports=2\n",
        (unsigned long long)worker_iteration_us(rate),
        (unsigned long long)s_log_suppressed_count,
        (unsigned long long)s_log_coalesced_count
    );
}

static void test_stop_leaves_pending_serial_report_unflushed(void)
{
    reset_world();
    reset_log_capture();
    s_fake_serial_clocklike_from_conversion = 2U;
    append_sample_ticks(12U);
    script(EV_STOP, 0U);

    const uint32_t allocations_before =
        ad7606_acquisition_test_heap_caps_allocations();
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(ad7606_acquisition_test_heap_caps_allocations() ==
        allocations_before + 1U);

    /* Current behavior: STOP drops no additional log and retains pending until next START. */
    assert(log_record_count(AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG) == 1U);
    assert(ad7606_acquisition_test_serial_diag_pending());
    assert(ad7606_acquisition_test_serial_diag_last_log_sample() == 1U);
    assert(s_log_suppressed_count == 11U);
    assert(s_log_coalesced_count == 1U);
    assert(s_log_coalesced_samples[0] == 9U);
    assert(!s_driver_live);
    assert(s_driver_deinits == 1U);
}

static void test_combined_diagnostic_boundary_and_slow_sink_completion(void)
{
    const uint32_t rate = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;
    const uint32_t injected_cost_us = 2390U;
    const char *flat_states[AD7606_CHANNEL_COUNT] = {
        "FLAT", "FLAT", "FLAT", "FLAT",
    };
    reset_world();
    reset_log_capture();
    /* Hide serial observations until the last sample so both reports coincide. */
    s_fake_serial_valid_after_conversion = rate;
    s_fake_diag_log_cost_us = injected_cost_us;
    s_fake_serial_log_cost_us = injected_cost_us;
    append_sample_ticks(rate);
    script(EV_STOP, 0U);

    const uint32_t allocations_before =
        ad7606_acquisition_test_heap_caps_allocations();
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(ad7606_acquisition_test_heap_caps_allocations() ==
        allocations_before + 1U);

    assert(log_record_count(AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG) == 1U);
    assert(log_record_count(AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL) ==
        AD7606_CHANNEL_COUNT);
    const test_log_record_t *serial = find_log_record(
        AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG,
        rate - 1U
    );
    assert(serial->pushed_sample_index == 0U);
    assert_diag_window_report(rate - 1U, 1U, flat_states);
    const test_log_record_t *diag = find_log_record(
        AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL,
        rate - 1U
    );
    assert(diag->pushed_sample_index == rate - 1U);
    assert(diag->pushed_timestamp_us == s_push_timestamp_by_sample[rate - 1U]);
    assert(worker_iteration_us(rate - 1U) == 5U * injected_cost_us);
    assert(s_last_synthetic_notification_batch == 5U);
    assert(s_last_push_sample_index == rate + 4U);
    assert(s_log_record_count == AD7606_CHANNEL_COUNT + 1U);

    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    /* Includes 499 intentional invalid observations plus 4 log-time collapses. */
    assert(status.collapsed_periods == rate - 1U + 4U);
    assert(!status.running);
    assert(!s_driver_live);
    printf(
        "MAX-366 virtual us: combined_boundary_iteration=%llu "
        "boundary_minus_steady=%llu notifications_coalesced=5 "
        "collapsed_conversions=4\n",
        (unsigned long long)worker_iteration_us(rate - 1U),
        (unsigned long long)worker_iteration_us(rate - 1U)
    );
}

static void test_unavailable_log_sink_does_not_block_stop(void)
{
    reset_world();
    reset_log_capture();
    s_fake_log_sink_available = false;
    append_sample_ticks(2U);
    script(EV_STOP, 0U);

    const uint32_t allocations_before =
        ad7606_acquisition_test_heap_caps_allocations();
    assert(ad7606_acquisition_start() == ESP_OK);
    assert(ad7606_acquisition_test_heap_caps_allocations() ==
        allocations_before + 1U);
    assert(s_log_sink_unavailable_count == 1U);
    assert(s_pushes == 2U);
    assert(!s_driver_live);
    assert(s_driver_deinits == 1U);
}

static void test_diag_fixed_seed_random_stream(void)
{
    uint32_t random_state = UINT32_C(0x6d2b79f5);
    diag_reset_both();
    for (size_t sample = 0U; sample < 2048U; ++sample) {
        int16_t raw[AD7606_CHANNEL_COUNT];
        for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
            random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
            raw[channel] = (int16_t)((int32_t)((random_state >> 16U) & UINT32_C(0xffff)) - 32768);
        }
        diag_push_values(raw[0], raw[1], raw[2], raw[3]);
        if ((sample % 127U) == 0U) diag_compare_with_reference();
    }
    diag_compare_with_reference();
}

/* Host timing cannot measure Xtensa instruction cost; ESP32 HIL cycle measurement is authoritative and was not run. */
static void test_diag_integer_accumulator_contract(void)
{
    test_diag_basic_values_and_channel_independence();
    test_diag_supported_rate_windows_and_overflow_bound();
    test_diag_report_reset_boundary();
    test_diag_window_report_semantics();
    test_diag_fixed_seed_random_stream();
    diag_reset_both();
    diag_compare_with_reference();
}

int main(void)
{
    test_diag_integer_accumulator_contract();
    test_diag_logging_virtual_time_characterization();
    test_serial_diag_rate_limit_boundary();
    test_stop_leaves_pending_serial_report_unflushed();
    test_combined_diagnostic_boundary_and_slow_sink_completion();
    test_unavailable_log_sink_does_not_block_stop();
    test_run_samples_then_stop_releases_everything();
    test_safe_os_mismatch_runs_and_is_reported();
    test_unknown_os_measurement_refuses_start();
    test_collapsed_periods_are_one_conversion();
    test_successful_read_jitter_statistics();
    test_jitter_bucket_boundaries_and_open_ended_bucket();
    test_negative_jitter_is_clamped_and_counted_early();
    test_jitter_counter_saturation();
    test_low_rail_refuses_start_without_touching_the_adc();
    test_unverified_rail_starts_with_warning();
    test_confirmed_rail_loss_stops_and_releases_the_adc();
    test_single_low_reading_does_not_stop();
    test_read_errors_are_counted_and_not_pushed();
    test_startup_failures_clean_up();
    test_restart_resets_cadence_counters();
    test_restart_clears_a_latched_fault();
    test_overcurrent_stops_and_latches();
    return 0;
}
