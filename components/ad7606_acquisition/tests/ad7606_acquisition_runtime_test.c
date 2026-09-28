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

typedef enum { EV_TICK, EV_STOP } event_type_t;
typedef struct {
    event_type_t type;
    uint32_t count;
    int64_t timestamp_adjustment_us;
} event_t;

static event_t s_script[64];
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
static float s_last_push[SEISMIC_CHANNEL_COUNT];
static seismic_source_t s_buffer_source;
static uint32_t s_buffer_rate;
static uint32_t s_ad7606_rate;
static bool s_running_seen_during_push;
static unsigned s_beats;
static unsigned s_notify_gives;
static unsigned s_notify_from_isr;
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
    s_buffer_source = SEISMIC_SOURCE_TEST;
    s_buffer_rate = 12345U;
    s_ad7606_rate = 0U;
    s_running_seen_during_push = true;
    s_beats = 0U;
    s_notify_gives = 0U;
    s_notify_from_isr = 0U;
    s_fake_pcnt_count = 0U;
    s_fake_busy_level = 0;
    s_fake_busy_intr_type = GPIO_INTR_DISABLE;
    s_last_busy_intr_edge = GPIO_INTR_DISABLE;
    s_fake_busy_isr = NULL;
    s_fake_busy_isr_args = NULL;
    s_fake_gpio_intr_enabled = false;
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
    if (higher_priority_task_woken != NULL) {
        *higher_priority_task_woken = pdFALSE;
    }
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
        for (uint32_t conversion = 0U; conversion < event.count; ++conversion) {
            s_fake_busy_level = 1;
            if (s_fake_gpio_intr_enabled &&
                s_fake_busy_intr_type == GPIO_INTR_POSEDGE && s_fake_busy_isr != NULL) {
                s_fake_busy_isr(s_fake_busy_isr_args);
            }
            s_fake_busy_level = 0;
            ++s_fake_pcnt_count;
            if (s_fake_gpio_intr_enabled &&
                s_fake_busy_intr_type == GPIO_INTR_NEGEDGE && s_fake_busy_isr != NULL) {
                s_fake_busy_isr(s_fake_busy_isr_args);
            }
        }
        s_now_us += PERIOD_US * (int64_t)event.count + event.timestamp_adjustment_us;
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
    observation->conversion_complete = true;
    observation->spi_transaction_ok = true;
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

int main(void)
{
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
