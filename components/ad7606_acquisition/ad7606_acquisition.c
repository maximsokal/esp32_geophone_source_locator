#include "ad7606_acquisition.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "ad7606_driver.h"
#include "ad7606_acquisition_timing.h"
#include "ad7606_diag_accum.h"
#include "ad7606_power_policy.h"
#include "board_config.h"
#include "ina3221_monitor.h"

#include "runtime_observability.h"
#include "app_task_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if SEISMIC_CHANNEL_COUNT != AD7606_SIGNAL_CHANNEL_COUNT
#error "AD7606 signal quality scope must match the application seismic channel count"
#endif

#define AD7606_DIAG_MAX_RAW_SQUARE (UINT64_C(1) << 30)
_Static_assert(
    CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 500U ||
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 5000U ||
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 10000U ||
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 20000U ||
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 40000U,
    "AD7606 diagnostic cadence must use a Kconfig-supported sample rate"
);
/* At 40 kHz, even 40000 samples of INT16_MIN^2 use only 42,949,672,960,000
 * of UINT64_MAX, leaving more than 429,000x headroom before overflow. */
_Static_assert(
    (uint64_t)AD7606_DIAG_MAX_WINDOW_SAMPLES * AD7606_DIAG_MAX_RAW_SQUARE <=
        UINT64_MAX,
    "AD7606 diagnostic sum of squares must fit in uint64_t"
);


static const char *TAG =
    "AD7606_ACQ";

static const char *DIAG_TAG =
    "AD7606_DIAG";


static TaskHandle_t s_task =
    NULL;

/* Keep task handles alive until every ISR/task notification drops its ref. */
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_running = false;
static bool s_stop_requested = false;
static bool s_starting = false;
static uint32_t s_task_users = 0U;

static int64_t s_sample_t0_us;
static uint64_t s_jitter_period_us;

static portMUX_TYPE s_status_lock =
    portMUX_INITIALIZER_UNLOCKED;

static ad7606_acquisition_status_t s_status;

/* MAX-242: rail watch state, owned by the Core 1 worker. */
static ad7606_rail_watch_t s_rail_watch;
static uint64_t s_rail_last_check_conversion = 0U;
#define AD7606_RAIL_CHECK_PERIOD_US 1000000LL


static ad7606_serial_diag_state_t s_serial_diag;
static uint8_t *s_serial_quality_window_flags;
static uint64_t s_serial_diag_last_log_sample = 0U;
static bool s_serial_diag_log_pending = false;

#if defined(AD7606_ACQUISITION_HOST_TEST)
static ad7606_acquisition_test_log_sink_t s_test_log_sink;
static uint32_t s_test_heap_caps_allocations;
static ad7606_acquisition_test_counters_t s_test_counters;
static uint32_t s_test_status_delay_us;

static void ad7606_test_log_event(
    ad7606_acquisition_test_log_event_t event,
    uint64_t sample_index,
    size_t channel,
    uint32_t count,
    const char *state
)
{
    if (s_test_log_sink != NULL) {
        (void)s_test_log_sink(event, sample_index, channel, count, state);
    }
}
#endif

#define AD7606_LEDC_XTAL_CLOCK_HZ 40000000U
/* Datasheet t3 CONVST high minimum is 25 ns; target 2.1 us for margin. */
#define AD7606_CONVST_TARGET_HIGH_NS 2100U
#define AD7606_ACQUISITION_BUSY_STOP_TIMEOUT_US 100U
#define AD7606_PCNT_HIGH_LIMIT 16384
#define AD7606_PCNT_LOW_LIMIT (-16384)

static pcnt_unit_handle_t s_pcnt_unit;
static pcnt_channel_handle_t s_pcnt_channel;
static bool s_pcnt_enabled;
static bool s_pcnt_started;
static bool s_busy_isr_added;
static bool s_busy_intr_enabled;
static bool s_ledc_timer_configured;
static bool s_ledc_channel_configured;
static bool s_driver_initialized;
static uint32_t s_ledc_duty_resolution;
static uint32_t s_ledc_duty_count;
static uint32_t s_convst_high_ns;
static ad7606_read_mode_t s_read_mode = AD7606_READ_MODE_AUTO;
static ad7606_count_extension_t s_pcnt_extension;
static ad7606_sample_accounting_t s_sample_accounting;


static bool ad7606_lifecycle_running(void)
{
    bool running;
    portENTER_CRITICAL(&s_lifecycle_lock);
    running = s_running;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return running;
}


static bool ad7606_lifecycle_stop_requested(void)
{
    bool stop_requested;
    portENTER_CRITICAL(&s_lifecycle_lock);
    stop_requested = s_stop_requested;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return stop_requested;
}


static TaskHandle_t ad7606_lifecycle_task(void)
{
    TaskHandle_t task;
    portENTER_CRITICAL(&s_lifecycle_lock);
    task = s_task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return task;
}


static bool ad7606_lifecycle_starting(void)
{
    bool starting;
    portENTER_CRITICAL(&s_lifecycle_lock);
    starting = s_starting;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return starting;
}


static bool ad7606_lifecycle_begin_start(void)
{
    bool started = false;
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_task == NULL && !s_starting) {
        s_starting = true;
        s_stop_requested = false;
        s_running = false;
        started = true;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return started;
}


static void ad7606_lifecycle_finish_start(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_starting = false;
    if (s_task == NULL) {
        s_running = false;
        s_stop_requested = false;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
}


static void ad7606_lifecycle_set_task(TaskHandle_t task)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_task = task;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}


static void ad7606_lifecycle_set_running(bool running)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_running = running;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}


static bool ad7606_lifecycle_start_running(void)
{
    bool started;
    portENTER_CRITICAL(&s_lifecycle_lock);
    started = !s_stop_requested;
    s_running = started;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return started;
}


static TaskHandle_t IRAM_ATTR ad7606_lifecycle_acquire_busy_isr(void)
{
    TaskHandle_t task;
    portENTER_CRITICAL_ISR(&s_lifecycle_lock);
    task = s_task;
    if (task != NULL && s_running) {
        ++s_task_users;
    } else {
        task = NULL;
    }
    portEXIT_CRITICAL_ISR(&s_lifecycle_lock);
    return task;
}


static void ad7606_lifecycle_release_task(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    --s_task_users;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}


static void IRAM_ATTR ad7606_lifecycle_release_busy_isr(void)
{
    portENTER_CRITICAL_ISR(&s_lifecycle_lock);
    --s_task_users;
    portEXIT_CRITICAL_ISR(&s_lifecycle_lock);
}


static void ad7606_lifecycle_request_stop(void)
{
    TaskHandle_t task;
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_running = false;
    task = s_task;
    if (task != NULL || s_starting) {
        s_stop_requested = true;
    }
    if (task != NULL) {
        ++s_task_users;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);

    if (task != NULL) {
        (void)xTaskNotifyGive(task);
        ad7606_lifecycle_release_task();
    }
}


static void ad7606_lifecycle_finish_worker(void)
{
    ad7606_lifecycle_set_running(false);
    for (;;) {
        uint32_t users;
        portENTER_CRITICAL(&s_lifecycle_lock);
        users = s_task_users;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        if (users == 0U) {
            break;
        }
        vTaskDelay(1U);
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_task = NULL;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}


static void ad7606_busy_isr(void *argument);


#if defined(AD7606_ACQUISITION_HOST_TEST)
void ad7606_acquisition_test_seed_lifecycle(TaskHandle_t task, bool running)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_task = task;
    s_running = running;
    s_stop_requested = false;
    s_starting = false;
    s_task_users = 0U;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

void ad7606_acquisition_test_busy_isr(void)
{
    ad7606_busy_isr(NULL);
}

void ad7606_acquisition_test_request_stop(void)
{
    ad7606_lifecycle_request_stop();
}

bool ad7606_acquisition_test_task_published(void)
{
    return ad7606_lifecycle_task() != NULL;
}

void ad7606_acquisition_test_worker_finish(void)
{
    ad7606_lifecycle_finish_worker();
}
#endif


typedef struct
{
    SemaphoreHandle_t ready;
    esp_err_t result;

} ad7606_start_context_t;


static ad7606_diag_channel_t s_diag[
    /* All four words belong to the connected geophone inputs V1-V4. */
    AD7606_CHANNEL_COUNT
];


static void ad7606_diag_reset(void)
{
    memset(s_diag, 0, sizeof(s_diag));

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        s_diag[channel].min_raw = INT16_MAX;
        s_diag[channel].max_raw = INT16_MIN;
    }
}


static void ad7606_diag_record(
    const ad7606_sample_t *sample
)
{
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        ad7606_diag_channel_record(&s_diag[channel], sample->raw[channel]);
    }
}


static const char *ad7606_diag_state(
    const ad7606_diag_channel_t *diag
)
{
    if (diag->saturation_count > 0U) {
        return "CLIP";
    }

    if (diag->count > 0U && diag->min_raw == diag->max_raw) {
        return "FLAT";
    }

    return "VAR";
}


static ad7606_diag_metrics_t ad7606_diag_measure(
    const ad7606_diag_channel_t *diag
)
{
    const double count = (double)diag->count;
    const double mean_raw = (double)diag->sum_raw / count;
    double variance_raw =
        ((double)diag->sum_sq_raw / count) -
        (mean_raw * mean_raw);

    /* Protect sqrt() from a tiny negative caused by rounding. */
    if (variance_raw < 0.0) {
        variance_raw = 0.0;
    }

    const double max_from_mean = fabs((double)diag->max_raw - mean_raw);
    const double min_from_mean = fabs((double)diag->min_raw - mean_raw);

    return (ad7606_diag_metrics_t) {
        .mean_raw = mean_raw,
        .rms_raw = sqrt(variance_raw),
        .p2p_raw = (double)diag->max_raw - (double)diag->min_raw,
        .peak_raw = max_from_mean > min_from_mean
            ? max_from_mean
            : min_from_mean,
    };
}


static void ad7606_diag_log_and_reset(uint64_t sample_index)
{
#if !defined(AD7606_ACQUISITION_HOST_TEST)
    (void)sample_index;
#endif
    /* One raw ADC code in millivolts for the configured bipolar range. */
    const double lsb_mv =
        (double)ad7606_raw_to_uv(1) /
        1000.0;

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        const ad7606_diag_channel_t *diag = &s_diag[channel];

        if (diag->count == 0U) {
#if defined(AD7606_ACQUISITION_HOST_TEST)
            ad7606_test_log_event(
                AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL,
                sample_index,
                channel,
                0U,
                "EMPTY"
            );
#endif
            ESP_LOGW(
                DIAG_TAG,
                "CH%u no samples",
                (unsigned)(channel + 1U)
            );
            continue;
        }

        const ad7606_diag_metrics_t metrics = ad7606_diag_measure(diag);
        const char *state = ad7606_diag_state(diag);
#if defined(AD7606_ACQUISITION_HOST_TEST)
        ad7606_test_log_event(
            AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL,
            sample_index,
            channel,
            diag->count,
            state
        );
#endif

        ESP_LOGI(
            DIAG_TAG,
            "CH%u n=%u mean=%+.3fmV rms=%.3fmV p2p=%.3fmV peak=%.3fmV min=%+.3fmV max=%+.3fmV sat=%u %s",
            (unsigned)(channel + 1U),
            (unsigned)diag->count,
            metrics.mean_raw * lsb_mv,
            metrics.rms_raw * lsb_mv,
            metrics.p2p_raw * lsb_mv,
            metrics.peak_raw * lsb_mv,
            (double)diag->min_raw * lsb_mv,
            (double)diag->max_raw * lsb_mv,
            (unsigned)diag->saturation_count,
            state
        );
    }

    ad7606_diag_reset();
}


static void ad7606_diag_finish_sample(uint64_t sample_index)
{
    if ((sample_index + 1U) % CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 0U) {
        ad7606_diag_log_and_reset(sample_index);
    }
}


#if defined(AD7606_ACQUISITION_HOST_TEST)
void ad7606_acquisition_test_diag_reset(void)
{
    ad7606_diag_reset();
}

void ad7606_acquisition_test_diag_record(const ad7606_sample_t *sample)
{
    ad7606_diag_record(sample);
}

void ad7606_acquisition_test_diag_snapshot(ad7606_diag_channel_t *out)
{
    memcpy(out, s_diag, sizeof(s_diag));
}

const char *ad7606_acquisition_test_diag_state(size_t channel)
{
    return ad7606_diag_state(&s_diag[channel]);
}

bool ad7606_acquisition_test_diag_metrics(
    size_t channel,
    ad7606_diag_metrics_t *out
)
{
    if (s_diag[channel].count == 0U) {
        return false;
    }
    *out = ad7606_diag_measure(&s_diag[channel]);
    return true;
}

void ad7606_acquisition_test_diag_finish_sample(uint64_t sample_index)
{
    ad7606_diag_finish_sample(sample_index);
}

void ad7606_acquisition_test_log_sink_set(
    ad7606_acquisition_test_log_sink_t sink
)
{
    s_test_log_sink = sink;
}

bool ad7606_acquisition_test_serial_diag_pending(void)
{
    return s_serial_diag_log_pending;
}

uint64_t ad7606_acquisition_test_serial_diag_last_log_sample(void)
{
    return s_serial_diag_last_log_sample;
}

uint32_t ad7606_acquisition_test_heap_caps_allocations(void)
{
    return s_test_heap_caps_allocations;
}

void ad7606_acquisition_test_counters_reset(void)
{
    memset(&s_test_counters, 0, sizeof(s_test_counters));
}

void ad7606_acquisition_test_counters_get(
    ad7606_acquisition_test_counters_t *out
)
{
    if (out != NULL) {
        *out = s_test_counters;
    }
}

void ad7606_acquisition_test_status_delay_set(uint32_t delay_us)
{
    s_test_status_delay_us = delay_us;
}
#endif


static void ad7606_status_reset(void)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );

    memset(
        &s_status,
        0,
        sizeof(s_status)
    );

    s_sample_t0_us = 0;
    s_jitter_period_us = 0U;

    s_status.serial_pattern =
        AD7606_SERIAL_PATTERN_UNKNOWN;

    s_status.serial_quality =
        AD7606_SERIAL_QUALITY_UNKNOWN;

    s_status.signal_quality =
        AD7606_SIGNAL_QUALITY_UNKNOWN;

    s_status.stream_state =
        AD7606_SERIAL_STREAM_UNKNOWN;

    s_status.busy_level =
        -1;

    s_status.os_ratio_configured = CONFIG_AD7606_OVERSAMPLING_RATIO;
    s_status.os_ratio_measured = 0U;

    portEXIT_CRITICAL(
        &s_status_lock
    );
}


static void ad7606_status_set_power(
    ad7606_power_gate_t gate,
    bool fault_latched,
    float rail_v
)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.power_gate = (int)gate;
    if (fault_latched) {
        s_status.power_fault_latched = true;
        s_status.power_fault_rail_v = rail_v;
    }
    portEXIT_CRITICAL(&s_status_lock);
}


static void ad7606_status_set_overcurrent(
    float rail_v,
    float current_a
)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.power_gate = (int)AD7606_POWER_GATE_OVERCURRENT;
    s_status.power_fault_latched = true;
    s_status.power_fault_rail_v = rail_v;
    s_status.power_fault_current_a = current_a;
    portEXIT_CRITICAL(&s_status_lock);
}


/* MAX-242: the INA3221 CH3 reading of the AD7606 +5 V rail. */
static void ad7606_read_rail(
    ina3221_monitor_rail_t *rail
)
{
    memset(rail, 0, sizeof(*rail));
    if (ina3221_monitor_get_rail(INA3221_MONITOR_RAIL_AD7606_5V, rail) != ESP_OK) {
        memset(rail, 0, sizeof(*rail));
    }
}


static void ad7606_status_set_running(
    bool running
)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );

    s_status.running =
        running;

    portEXIT_CRITICAL(
        &s_status_lock
    );
}


static void ad7606_status_record_conversion_count(uint64_t conversion_count)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );
    s_status.timer_ticks = conversion_count;
    portEXIT_CRITICAL(&s_status_lock);
}


static void ad7606_status_record_attempt(void)
{
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.conversion_attempts = ad7606_acquisition_saturating_add_u64(
        s_status.conversion_attempts,
        1U
    );
    portEXIT_CRITICAL(&s_status_lock);
}


static void ad7606_status_record_gaps(uint64_t missed_conversions)
{
    if (missed_conversions == 0U) {
        return;
    }
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.collapsed_periods = ad7606_acquisition_saturating_add_u64(
        s_status.collapsed_periods,
        missed_conversions
    );
    portEXIT_CRITICAL(&s_status_lock);
}


static unsigned ad7606_jitter_bucket(
    uint64_t lateness_us
)
{
    unsigned bucket = 0U;
    while (
        bucket + 1U < AD7606_ACQUISITION_JITTER_BUCKET_COUNT &&
        lateness_us >= (UINT64_C(1) << (bucket + 1U))
    ) {
        ++bucket;
    }
    return bucket;
}


static uint32_t ad7606_status_record_failure(void)
{
    uint32_t consecutive_failures;

#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );

    s_status.read_errors++;
    s_status.consecutive_failures++;

    consecutive_failures =
        s_status.consecutive_failures;

    portEXIT_CRITICAL(
        &s_status_lock
    );

    return consecutive_failures;
}


static void ad7606_status_record_sample(
    const ad7606_sample_t *sample,
    const float latest_uv[AD7606_CHANNEL_COUNT],
    const ad7606_serial_diag_state_t *serial_diag,
    int busy_level,
    int64_t scheduled_us
)
{
    const bool jitter_available = scheduled_us >= 0;
    const bool early = jitter_available && sample->timestamp_us < scheduled_us;
    const uint64_t lateness_us = !jitter_available || early
        ? 0U
        : (uint64_t)sample->timestamp_us - (uint64_t)scheduled_us;
    const unsigned jitter_bucket = ad7606_jitter_bucket(lateness_us);

#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
    ++s_test_counters.sample_status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );

#if defined(AD7606_ACQUISITION_HOST_TEST)
    if (s_test_status_delay_us != 0U) {
        esp_rom_delay_us(s_test_status_delay_us);
    }
#endif

    if (jitter_available) {
        ad7606_acquisition_jitter_t *jitter = &s_status.jitter;
        if (jitter->count == 0U) {
            jitter->min_us = lateness_us;
            jitter->max_us = lateness_us;
        }
        else {
            if (lateness_us < jitter->min_us) {
                jitter->min_us = lateness_us;
            }
            if (lateness_us > jitter->max_us) {
                jitter->max_us = lateness_us;
            }
        }
        jitter->count = ad7606_acquisition_saturating_add_u32(jitter->count, 1U);
        if (early) {
            jitter->early_count = ad7606_acquisition_saturating_add_u32(
                jitter->early_count,
                1U
            );
        }
        jitter->sum_us = ad7606_acquisition_saturating_add_u64(
            jitter->sum_us,
            lateness_us
        );
        jitter->histogram_us[jitter_bucket] =
            ad7606_acquisition_saturating_add_u32(
                jitter->histogram_us[jitter_bucket],
                1U
            );
    }
    if (
        s_status.latest_valid &&
        sample->timestamp_us > s_status.last_sample_timestamp_us
    ) {
        const uint64_t interval_us =
            (uint64_t)(
                sample->timestamp_us -
                s_status.last_sample_timestamp_us
            );

        s_status.last_successful_sample_interval_us =
            interval_us;

        if (interval_us > s_status.max_successful_sample_interval_us) {
            s_status.max_successful_sample_interval_us =
                interval_us;
        }
    }
    else if (s_status.latest_valid) {
        /* Keep invalid/non-monotonic timestamps deterministic and visible. */
        s_status.last_successful_sample_interval_us =
            0U;
    }

    s_status.samples++;

    s_status.consecutive_failures =
        0;

    s_status.serial_suspicious =
        serial_diag->serial_suspicious;

    s_status.serial_pattern =
        serial_diag->pattern;

    s_status.serial_quality =
        serial_diag->quality;

    s_status.signal_quality =
        serial_diag->signal_quality;

    s_status.stream_state =
        serial_diag->stream_state;

    s_status.stream_stuck =
        serial_diag->stream_stuck;

    s_status.suspicious_streak =
        serial_diag->suspicious_streak;

    s_status.repeated_frame_streak =
        serial_diag->repeated_frame_streak;

    s_status.all_zero_frames =
        serial_diag->all_zero_frames;

    s_status.all_ff_frames =
        serial_diag->all_ff_frames;

    s_status.all_identical_frames =
        serial_diag->all_identical_frames;

    s_status.varied_frames =
        serial_diag->varied_frames;

    s_status.digital_interference_frames =
        serial_diag->digital_interference_frames;

    s_status.signal_rail_hit_frames =
        serial_diag->signal_rail_hit_frames;

    s_status.signal_clip_frames =
        serial_diag->signal_clip_frames;
    s_status.quality_window_frames =
        serial_diag->quality_window_frames;
    s_status.quality_window_bad_frames =
        serial_diag->quality_window_bad_frames;
    s_status.quality_window_all_zero_frames =
        serial_diag->quality_window_all_zero_frames;
    s_status.quality_window_all_ff_frames =
        serial_diag->quality_window_all_ff_frames;
    s_status.quality_window_all_identical_frames =
        serial_diag->quality_window_all_identical_frames;
    s_status.quality_window_repeated_frames =
        serial_diag->quality_window_repeated_frames;
    s_status.quality_window_digital_interference_frames =
        serial_diag->quality_window_digital_interference_frames;
    s_status.signal_quality_window_rail_hit_frames =
        serial_diag->signal_quality_window_rail_hit_frames;

    s_status.signal_quality_window_clip_frames =
        serial_diag->signal_quality_window_clip_frames;

    s_status.busy_level =
        busy_level;

    s_status.latest_valid =
        true;

    s_status.last_sample_timestamp_us =
        sample->timestamp_us;

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        s_status.latest_uv[channel] =
            latest_uv[channel];
    }

    portEXIT_CRITICAL(
        &s_status_lock
    );
}


static void ad7606_serial_diag_log_frame(
    const ad7606_serial_observation_t *observation,
    const ad7606_serial_diag_state_t *serial_diag,
    const ad7606_serial_diag_event_t *event,
    uint64_t samples
);


static bool ad7606_account_sample_index(uint64_t sample_index, bool sample_valid)
{
    uint64_t missed = 0U;
    const bool accepted = ad7606_acquisition_account_sample(
        &s_sample_accounting,
        sample_index,
        sample_valid,
        &missed
    );
    ad7606_status_record_gaps(missed);
    return accepted;
}


static esp_err_t ad7606_publish_sample(
    uint64_t sample_index,
    ad7606_sample_t *sample,
    const ad7606_serial_observation_t *serial_observation,
    uint64_t *successful_samples
)
{
    if (
        sample == NULL ||
        serial_observation == NULL ||
        successful_samples == NULL ||
        !ad7606_acquisition_timestamp_from_index(
            s_sample_t0_us,
            sample_index,
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
            &sample->timestamp_us
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ad7606_account_sample_index(sample_index, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    ad7606_serial_diag_event_t serial_event;
    (void)ad7606_serial_diag_observe(
        &s_serial_diag,
        serial_observation->rx,
        &serial_event
    );

    float all_channels_uv[AD7606_CHANNEL_COUNT];
    for (size_t channel = 0U; channel < AD7606_CHANNEL_COUNT; ++channel) {
        all_channels_uv[channel] = ad7606_raw_to_uv(sample->raw[channel]);
    }

    ad7606_status_record_sample(
        sample,
        all_channels_uv,
        &s_serial_diag,
        serial_observation->busy_level,
        -1
    );
    ad7606_serial_diag_log_frame(
        serial_observation,
        &s_serial_diag,
        &serial_event,
        sample_index + 1U
    );
    ad7606_diag_record(sample);

    float seismic_channels[SEISMIC_CHANNEL_COUNT];
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        seismic_channels[channel] = all_channels_uv[channel];
    }

    const esp_err_t err = seismic_buffer_push_indexed(
        sample->timestamp_us,
        sample_index,
        seismic_channels
    );
    if (err != ESP_OK) {
        ad7606_status_record_gaps(1U);
        ESP_LOGE(TAG, "Seismic buffer push failed: %s", esp_err_to_name(err));
        return err;
    }

    ++*successful_samples;
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, *successful_samples);
    ad7606_diag_finish_sample(sample_index);
    return ESP_OK;
}


static void ad7606_serial_diag_log_frame(
    const ad7606_serial_observation_t *observation,
    const ad7606_serial_diag_state_t *serial_diag,
    const ad7606_serial_diag_event_t *event,
    uint64_t samples
)
{
    const bool periodic_due =
        serial_diag->suspicious_streak > 0U &&
        (
            s_serial_diag_last_log_sample == 0U ||
            samples - s_serial_diag_last_log_sample >=
                CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
        );

    if (
        event->pattern_changed ||
        event->abnormal_started ||
        event->suspicious_started ||
        event->quality_changed ||
        event->signal_quality_changed ||
        event->stream_state_changed ||
        periodic_due
    ) {
        if (s_serial_diag_log_pending) {
#if defined(AD7606_ACQUISITION_HOST_TEST)
            ad7606_test_log_event(
                AD7606_ACQUISITION_TEST_LOG_SERIAL_COALESCED,
                samples - 1U,
                SIZE_MAX,
                0U,
                NULL
            );
#endif
        }
        s_serial_diag_log_pending = true;
    }

    if (!s_serial_diag_log_pending) {
        return;
    }

    if (
        s_serial_diag_last_log_sample != 0U &&
        samples - s_serial_diag_last_log_sample <
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    ) {
#if defined(AD7606_ACQUISITION_HOST_TEST)
        ad7606_test_log_event(
            AD7606_ACQUISITION_TEST_LOG_SERIAL_SUPPRESSED,
            samples - 1U,
            SIZE_MAX,
            0U,
            NULL
        );
#endif
        return;
    }

    int16_t words[AD7606_SERIAL_WORD_COUNT] = {0};
    (void)ad7606_serial_decode_frame(
        observation->rx,
        words
    );

#if defined(AD7606_ACQUISITION_HOST_TEST)
    ad7606_test_log_event(
        AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG,
        samples - 1U,
        SIZE_MAX,
        0U,
        serial_diag->serial_suspicious ? "SUSPICIOUS" : "NORMAL"
    );
#endif
    ESP_LOGI(
        DIAG_TAG,
        "AD7606_SERIAL_DIAG: rx=%02X %02X %02X %02X %02X %02X %02X %02X words=[%d,%d,%d,%d] class=%s transport_quality=%s signal_quality=%s stream_state=%s stream_stuck=%s busy=%d samples=%" PRIu64 " streak=%u repeated_streak=%u all_zero=%u all_ff=%u identical=%u varied=%u digital_interference=%u signal_rail_hits=%u signal_clips=%u window=%u transport_bad=%u all_zero=%u all_ff=%u identical=%u repeated=%u digital=%u signal_rail=%u signal_clip=%u serial_suspicious=%s",
        observation->rx[0],
        observation->rx[1],
        observation->rx[2],
        observation->rx[3],
        observation->rx[4],
        observation->rx[5],
        observation->rx[6],
        observation->rx[7],
        words[0],
        words[1],
        words[2],
        words[3],
        ad7606_serial_pattern_name(
            serial_diag->pattern
        ),
        ad7606_serial_quality_name(serial_diag->quality),
        ad7606_signal_quality_name(serial_diag->signal_quality),
        ad7606_serial_stream_state_name(serial_diag->stream_state),
        serial_diag->stream_stuck ? "yes" : "no",
        observation->busy_level,
        samples,
        (unsigned)serial_diag->suspicious_streak,
        (unsigned)serial_diag->repeated_frame_streak,
        (unsigned)serial_diag->all_zero_frames,
        (unsigned)serial_diag->all_ff_frames,
        (unsigned)serial_diag->all_identical_frames,
        (unsigned)serial_diag->varied_frames,
        (unsigned)serial_diag->digital_interference_frames,
        (unsigned)serial_diag->signal_rail_hit_frames,
        (unsigned)serial_diag->signal_clip_frames,
        (unsigned)serial_diag->quality_window_frames,
        (unsigned)serial_diag->quality_window_bad_frames,
        (unsigned)serial_diag->quality_window_all_zero_frames,
        (unsigned)serial_diag->quality_window_all_ff_frames,
        (unsigned)serial_diag->quality_window_all_identical_frames,
        (unsigned)serial_diag->quality_window_repeated_frames,
        (unsigned)serial_diag->quality_window_digital_interference_frames,
        (unsigned)serial_diag->signal_quality_window_rail_hit_frames,
        (unsigned)serial_diag->signal_quality_window_clip_frames,
        serial_diag->serial_suspicious ? "yes" : "no"
    );

    s_serial_diag_last_log_sample = samples;
    s_serial_diag_log_pending = false;
}


static void IRAM_ATTR ad7606_busy_isr(void *argument)
{
    (void)argument;
    TaskHandle_t task = ad7606_lifecycle_acquire_busy_isr();
    if (task != NULL) {
        BaseType_t higher_priority_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_woken);
        ad7606_lifecycle_release_busy_isr();
        if (higher_priority_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}


static ad7606_read_mode_t ad7606_requested_read_mode(void)
{
#if CONFIG_AD7606_READ_MODE_AFTER_CONVERSION
    return AD7606_READ_MODE_AFTER_CONVERSION;
#elif CONFIG_AD7606_READ_MODE_DURING_CONVERSION
    return AD7606_READ_MODE_DURING_CONVERSION;
#else
    return AD7606_READ_MODE_AUTO;
#endif
}


static const char *ad7606_read_mode_name(ad7606_read_mode_t mode)
{
    switch (mode) {
        case AD7606_READ_MODE_AFTER_CONVERSION: return "after_conversion";
        case AD7606_READ_MODE_DURING_CONVERSION: return "during_conversion";
        case AD7606_READ_MODE_AUTO: default: return "auto";
    }
}


static esp_err_t ad7606_prepare_timing(void)
{
    const uint32_t max_resolution = ledc_find_suitable_duty_resolution(
        AD7606_LEDC_XTAL_CLOCK_HZ,
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    );
    s_ledc_duty_resolution = ad7606_acquisition_ledc_exact_resolution(
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        AD7606_LEDC_XTAL_CLOCK_HZ,
        max_resolution
    );
    if (s_ledc_duty_resolution == 0U) {
        ESP_LOGE(TAG, "XTAL LEDC cannot generate %u Hz",
            (unsigned)CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ);
        return ESP_ERR_INVALID_ARG;
    }

    s_ledc_duty_count = ad7606_acquisition_ledc_duty_count(
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        s_ledc_duty_resolution,
        AD7606_CONVST_TARGET_HIGH_NS,
        &s_convst_high_ns
    );
    if (s_ledc_duty_count == 0U || s_convst_high_ns < 1000U) {
        ESP_LOGE(TAG, "LEDC CONVST pulse calculation failed");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}


static esp_err_t ad7606_resolve_measured_timing(
    uint8_t measured_os,
    uint32_t measured_tconv_ns
)
{
    const bool unknown_degraded = measured_os == 0U &&
        ad7606_acquisition_tconv_is_unknown_degraded_window(measured_tconv_ns);
    const bool os_mismatch = measured_os != 0U &&
        measured_os != CONFIG_AD7606_OVERSAMPLING_RATIO;
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.os_ratio_configured = CONFIG_AD7606_OVERSAMPLING_RATIO;
    s_status.os_ratio_measured = measured_os;
    s_status.os_mismatch = os_mismatch;
    portEXIT_CRITICAL(&s_status_lock);
    if (measured_os == 0U && !unknown_degraded) {
        ESP_LOGE(TAG,
            "AD7606_OS_UNKNOWN: configured=OS%u measured=unknown tCONV=%u ns is outside calibrated bands",
            (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
            (unsigned)measured_tconv_ns);
#if defined(AD7606_ACQUISITION_HOST_TEST)
        ad7606_test_log_event(
            AD7606_ACQUISITION_TEST_LOG_OS_RESOLUTION,
            0U,
            measured_os,
            measured_tconv_ns,
            "ERROR AD7606_OS_UNKNOWN"
        );
#endif
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (unknown_degraded) {
        ESP_LOGW(TAG,
            "AD7606_OS_UNKNOWN_DEGRADED: configured=OS%u measured=unknown tCONV=%u ns; using configured timing",
            (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
            (unsigned)measured_tconv_ns);
#if defined(AD7606_ACQUISITION_HOST_TEST)
        ad7606_test_log_event(
            AD7606_ACQUISITION_TEST_LOG_OS_RESOLUTION,
            0U,
            measured_os,
            measured_tconv_ns,
            "WARN AD7606_OS_UNKNOWN_DEGRADED"
        );
#endif
    }
    if (os_mismatch) {
        ESP_LOGW(TAG, "AD7606_OS_MISMATCH: configured=OS%u measured=OS%u tCONV=%u ns",
            (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
            (unsigned)measured_os,
            (unsigned)measured_tconv_ns);
#if defined(AD7606_ACQUISITION_HOST_TEST)
        ad7606_test_log_event(
            AD7606_ACQUISITION_TEST_LOG_OS_RESOLUTION,
            0U,
            measured_os,
            measured_tconv_ns,
            "WARN AD7606_OS_MISMATCH"
        );
#endif
    }

    ad7606_timing_result_t timing;
    const bool timing_valid = unknown_degraded
        ? ad7606_acquisition_timing_resolve(
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
            CONFIG_AD7606_OVERSAMPLING_RATIO,
            CONFIG_AD7606_SPI_CLOCK_HZ,
            ad7606_requested_read_mode(),
            s_convst_high_ns,
            &timing)
        : ad7606_acquisition_timing_resolve_measured(
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
            CONFIG_AD7606_OVERSAMPLING_RATIO,
            measured_os,
            CONFIG_AD7606_SPI_CLOCK_HZ,
            ad7606_requested_read_mode(),
            s_convst_high_ns,
            &timing);
    if (!timing_valid) {
        ESP_LOGE(TAG,
            "Measured timing unsafe or read mode differs: rate=%u configured=OS%u "
            "measured=OS%u SCLK=%u requested=%s tCONV=%u ns",
            (unsigned)CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
            (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
            (unsigned)measured_os,
            (unsigned)CONFIG_AD7606_SPI_CLOCK_HZ,
            ad7606_read_mode_name(ad7606_requested_read_mode()),
            (unsigned)measured_tconv_ns);
        return ESP_ERR_INVALID_ARG;
    }

    s_read_mode = timing.mode;
    ESP_LOGI(TAG,
        "Timing valid: rate=%u Hz configured=OS%u measured=OS%u tCONV=%u ns "
        "SCLK=%u read=%" PRIu64 " ns tCONV(max)=%u ns mode=%s CONVST_high=%u ns",
        (unsigned)CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
        (unsigned)measured_os,
        (unsigned)measured_tconv_ns,
        (unsigned)CONFIG_AD7606_SPI_CLOCK_HZ,
        timing.read_time_ns,
        (unsigned)timing.tconv_max_ns,
        ad7606_read_mode_name(s_read_mode),
        (unsigned)timing.convst_high_ns);
    return ESP_OK;
}


static esp_err_t ad7606_counter_init(void)
{
    const pcnt_unit_config_t unit_config = {
        .low_limit = AD7606_PCNT_LOW_LIMIT,
        .high_limit = AD7606_PCNT_HIGH_LIMIT,
        .flags.accum_count = 1,
    };
    esp_err_t err = pcnt_new_unit(&unit_config, &s_pcnt_unit);
    if (err != ESP_OK) {
        return err;
    }

    err = pcnt_unit_add_watch_point(s_pcnt_unit, AD7606_PCNT_HIGH_LIMIT);
    if (err == ESP_OK) {
        err = pcnt_unit_add_watch_point(s_pcnt_unit, AD7606_PCNT_LOW_LIMIT);
    }
    if (err != ESP_OK) {
        return err;
    }

    const pcnt_chan_config_t channel_config = {
        .edge_gpio_num = BOARD_AD7606_BUSY_GPIO,
        .level_gpio_num = -1,
    };
    err = pcnt_new_channel(s_pcnt_unit, &channel_config, &s_pcnt_channel);
    if (err == ESP_OK) {
        err = pcnt_channel_set_edge_action(
            s_pcnt_channel,
            PCNT_CHANNEL_EDGE_ACTION_HOLD,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE
        );
    }
    if (err == ESP_OK) {
        err = pcnt_channel_set_level_action(
            s_pcnt_channel,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP
        );
    }
    if (err == ESP_OK) {
        err = pcnt_unit_clear_count(s_pcnt_unit);
    }
    if (err == ESP_OK) {
        err = pcnt_unit_enable(s_pcnt_unit);
        s_pcnt_enabled = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = pcnt_unit_start(s_pcnt_unit);
        s_pcnt_started = err == ESP_OK;
    }
    if (err == ESP_OK) {
        ad7606_acquisition_count_extension_reset(&s_pcnt_extension);
    }
    return err;
}


static esp_err_t ad7606_counter_verify_self_test(void)
{
    int count = 0;
    esp_err_t err = pcnt_unit_get_count(s_pcnt_unit, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (count <= 0) {
        ESP_LOGE(TAG, "PCNT did not observe the AD7606 self-test BUSY pulse on GPIO%d",
            BOARD_AD7606_BUSY_GPIO);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(
        TAG,
        "PCNT BUSY input GPIO%d shares the interrupt pad through GPIO matrix; "
        "self-test pulses=%d",
        BOARD_AD7606_BUSY_GPIO,
        count
    );
    err = pcnt_unit_clear_count(s_pcnt_unit);
    if (err == ESP_OK) {
        ad7606_acquisition_count_extension_reset(&s_pcnt_extension);
        uint64_t ignored_count;
        (void)ad7606_acquisition_count_extend_u32(&s_pcnt_extension, 0U, &ignored_count);
    }
    return err;
}


static esp_err_t ad7606_busy_interrupt_start(void)
{
    esp_err_t err = board_gpio_isr_service_install();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IRAM GPIO ISR service unavailable: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_intr_type(
        BOARD_AD7606_BUSY_GPIO,
        s_read_mode == AD7606_READ_MODE_AFTER_CONVERSION
            ? GPIO_INTR_NEGEDGE
            : GPIO_INTR_POSEDGE
    );
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(BOARD_AD7606_BUSY_GPIO, ad7606_busy_isr, NULL);
        s_busy_isr_added = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = gpio_intr_enable(BOARD_AD7606_BUSY_GPIO);
        s_busy_intr_enabled = err == ESP_OK;
    }
    return err;
}


static esp_err_t ad7606_ledc_start(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)s_ledc_duty_resolution,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }
    s_ledc_timer_configured = true;

    const ledc_channel_config_t channel_config = {
        .gpio_num = BOARD_AD7606_CONVST_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = s_ledc_duty_count,
        .hpoint = 0,
    };
    s_sample_t0_us = esp_timer_get_time();
    if (!ad7606_lifecycle_start_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ad7606_lifecycle_set_running(false);
        return err;
    }
    s_ledc_channel_configured = true;
    if (ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0) !=
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ) {
        ad7606_lifecycle_set_running(false);
        ESP_LOGE(TAG, "LEDC XTAL output is not exactly %u Hz",
            (unsigned)CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}


static esp_err_t ad7606_read_conversion_count(uint64_t *conversion_count)
{
    if (conversion_count == NULL || s_pcnt_unit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_count = 0;
    esp_err_t err = pcnt_unit_get_count(s_pcnt_unit, &raw_count);
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t previous_count = s_pcnt_extension.extended_count;
    if (!ad7606_acquisition_count_extend_u32(
            &s_pcnt_extension,
            (uint32_t)raw_count,
            conversion_count)) {
        ESP_LOGE(TAG, "PCNT count moved backwards; stopping acquisition");
        return ESP_ERR_INVALID_STATE;
    }
    if (*conversion_count > previous_count) {
        ad7606_driver_presence_note_busy_pulse();
    }
    ad7606_status_record_conversion_count(*conversion_count);
    return ESP_OK;
}


static void ad7606_wait_busy_idle(void)
{
    const uint32_t timeout_us =
        (s_pcnt_unit != NULL ? 140U : AD7606_ACQUISITION_BUSY_STOP_TIMEOUT_US);
    for (uint32_t waited = 0U; waited < timeout_us; ++waited) {
        if (gpio_get_level(BOARD_AD7606_BUSY_GPIO) == 0) {
            return;
        }
        esp_rom_delay_us(1U);
    }
    ESP_LOGW(TAG, "BUSY remained high while stopping; final sample may be a gap");
}


static void ad7606_hardware_stop(void)
{
    ad7606_lifecycle_set_running(false);
    if (s_ledc_channel_configured) {
        const uint32_t pulse_wait_limit_us =
            (1000000U / CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ) + 5U;
        for (uint32_t waited = 0U;
             gpio_get_level(BOARD_AD7606_CONVST_GPIO) != 0 &&
                 waited < pulse_wait_limit_us;
             ++waited) {
            esp_rom_delay_us(1U);
        }
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        s_ledc_channel_configured = false;
    }
    if (s_busy_intr_enabled) {
        (void)gpio_intr_disable(BOARD_AD7606_BUSY_GPIO);
        s_busy_intr_enabled = false;
    }
    if (s_busy_isr_added) {
        (void)gpio_isr_handler_remove(BOARD_AD7606_BUSY_GPIO);
        s_busy_isr_added = false;
    }
    (void)gpio_set_intr_type(BOARD_AD7606_BUSY_GPIO, GPIO_INTR_DISABLE);
    if (s_pcnt_started) {
        ad7606_wait_busy_idle();
    }
}


static void ad7606_worker_cleanup(bool drain_latest, uint64_t *successful_samples)
{
    ad7606_hardware_stop();
    if (drain_latest && s_driver_initialized && successful_samples != NULL) {
        uint64_t conversion_count = 0U;
        if (ad7606_read_conversion_count(&conversion_count) == ESP_OK &&
            conversion_count > 0U &&
            gpio_get_level(BOARD_AD7606_BUSY_GPIO) == 0) {
            const uint64_t sample_index = conversion_count - 1U;
            if (sample_index >= s_sample_accounting.next_sample_index) {
                ad7606_sample_t sample = {0};
                ad7606_status_record_attempt();
                esp_err_t err = ad7606_driver_read_result(&sample);
                const int busy_after = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
                if (err == ESP_OK && busy_after == 0) {
                    ad7606_serial_observation_t observation;
                    err = ad7606_driver_get_last_serial_observation(&observation);
                    if (err == ESP_OK && observation.spi_transaction_ok) {
                        observation.busy_level = busy_after;
                        (void)ad7606_publish_sample(
                            sample_index,
                            &sample,
                            &observation,
                            successful_samples
                        );
                    }
                }
                if (err != ESP_OK || busy_after != 0) {
                    (void)ad7606_account_sample_index(sample_index, false);
                    (void)ad7606_status_record_failure();
                }
            }
        }
    }
    if (s_pcnt_started) {
        (void)pcnt_unit_stop(s_pcnt_unit);
        s_pcnt_started = false;
    }
    if (s_pcnt_enabled) {
        (void)pcnt_unit_disable(s_pcnt_unit);
        s_pcnt_enabled = false;
    }
    if (s_pcnt_channel != NULL) {
        (void)pcnt_del_channel(s_pcnt_channel);
        s_pcnt_channel = NULL;
    }
    if (s_pcnt_unit != NULL) {
        (void)pcnt_del_unit(s_pcnt_unit);
        s_pcnt_unit = NULL;
    }
    if (s_ledc_timer_configured) {
        (void)ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
        const ledc_timer_config_t deconfigure = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .deconfigure = true,
        };
        (void)ledc_timer_config(&deconfigure);
        s_ledc_timer_configured = false;
    }

    ad7606_driver_deinit();
    s_driver_initialized = false;
    ad7606_driver_presence_use_external_busy_isr(false);
    if (s_serial_quality_window_flags != NULL) {
        heap_caps_free(s_serial_quality_window_flags);
        s_serial_quality_window_flags = NULL;
    }
    (void)seismic_buffer_configure(SEISMIC_SOURCE_NONE, 0U);
    ad7606_status_set_running(false);
}


static esp_err_t ad7606_worker_initialize(void)
{
    esp_err_t err = ad7606_prepare_timing();
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t quality_window_frames = ad7606_serial_quality_window_frames(
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    );
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_heap_caps_allocations;
#endif
    s_serial_quality_window_flags = heap_caps_malloc(
        quality_window_frames,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_serial_quality_window_flags == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ad7606_serial_diag_reset(
        &s_serial_diag,
        s_serial_quality_window_flags,
        quality_window_frames
    );
    ad7606_acquisition_count_extension_reset(&s_pcnt_extension);
    s_sample_accounting = (ad7606_sample_accounting_t){0};
    ad7606_diag_reset();
    s_serial_diag_last_log_sample = 0U;
    s_serial_diag_log_pending = false;

    err = ad7606_counter_init();
    if (err != ESP_OK) {
        return err;
    }

    ad7606_driver_presence_use_external_busy_isr(true);
    err = ad7606_driver_init();
    if (err != ESP_OK) {
        return err;
    }
    s_driver_initialized = true;
    uint32_t measured_tconv_ns = 0U;
    err = ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AD7606_TCONV_MEASURED unavailable: %s", esp_err_to_name(err));
        return err;
    }
    const uint8_t measured_os = ad7606_acquisition_classify_tconv_ns(measured_tconv_ns);
    err = ad7606_resolve_measured_timing(measured_os, measured_tconv_ns);
    if (err != ESP_OK) {
        return err;
    }
    err = ad7606_counter_verify_self_test();
    if (err != ESP_OK) {
        return err;
    }
    s_rail_last_check_conversion = 0U;

    err = seismic_buffer_configure(
        SEISMIC_SOURCE_AD7606,
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    );
    if (err != ESP_OK) {
        return err;
    }
    err = ad7606_busy_interrupt_start();
    if (err != ESP_OK) {
        return err;
    }
    err = ad7606_ledc_start();
    if (err != ESP_OK) {
        return err;
    }

    s_jitter_period_us = ad7606_acquisition_period_us(
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    );
#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(&s_status_lock);
    s_status.jitter.period_us = s_jitter_period_us;
    portEXIT_CRITICAL(&s_status_lock);
    ad7606_status_set_running(true);
    ESP_LOGI(
        TAG,
        "Started: %u Hz, 4 channels, SPI3=%u Hz, OS configured=%u measured=%u mismatch=%s mode=%s, "
        "LEDC XTAL, BUSY GPIO=%d PCNT, core=%d",
        (unsigned)CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        (unsigned)CONFIG_AD7606_SPI_CLOCK_HZ,
        (unsigned)CONFIG_AD7606_OVERSAMPLING_RATIO,
        (unsigned)measured_os,
        measured_os != CONFIG_AD7606_OVERSAMPLING_RATIO ? "yes" : "no",
        ad7606_read_mode_name(s_read_mode),
        BOARD_AD7606_BUSY_GPIO,
        APP_TASK_CORE_REALTIME
    );
    return ESP_OK;
}


static void ad7606_worker(
    void *argument
)
{
    ad7606_start_context_t *start_context = (ad7606_start_context_t *)argument;
    ad7606_lifecycle_set_task(xTaskGetCurrentTaskHandle());

    if (ad7606_lifecycle_stop_requested()) {
        ad7606_lifecycle_finish_worker();
        start_context->result = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(start_context->ready);
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t startup_err = ad7606_worker_initialize();
    if (startup_err != ESP_OK) {
        ESP_LOGE(TAG, "Core 1 acquisition initialization failed: %s",
            esp_err_to_name(startup_err));
        ad7606_worker_cleanup(false, NULL);
        ad7606_lifecycle_finish_worker();
        start_context->result = startup_err;
        xSemaphoreGive(start_context->ready);
        vTaskDelete(NULL);
        return;
    }

    start_context->result = ESP_OK;
    xSemaphoreGive(start_context->ready);
    start_context = NULL;

    uint64_t successful_samples = 0U;
    runtime_observability_set_health_phase(
        RUNTIME_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE
    );
    while (ad7606_lifecycle_running()) {
        const uint32_t notification_count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!ad7606_lifecycle_running()) {
            break;
        }
        if (notification_count == 0U) {
            continue;
        }

        uint64_t conversion_count = 0U;
        esp_err_t err = ad7606_read_conversion_count(&conversion_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PCNT read failed: %s", esp_err_to_name(err));
            ad7606_lifecycle_set_running(false);
            break;
        }

#if defined(CONFIG_SEISMIC_AD7606_RAIL_INTERLOCK)
        if (conversion_count - s_rail_last_check_conversion >=
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ) {
            s_rail_last_check_conversion = conversion_count;
            const int64_t now_us = esp_timer_get_time();
            ina3221_monitor_rail_t rail;
            ad7606_read_rail(&rail);
            if (rail.overcurrent_latched) {
                ESP_LOGE(TAG,
                    "AD7606 +5 V rail over-current (INA3221 CH3 %.3f A): "
                    "stopping acquisition and releasing AD7606 pins to Hi-Z",
                    (double)rail.overcurrent_latched_current_a);
                ad7606_status_set_overcurrent(
                    rail.bus_voltage_v, rail.overcurrent_latched_current_a);
                ad7606_lifecycle_set_running(false);
                break;
            }
            if (ad7606_rail_watch_update(
                    &s_rail_watch, rail.available, rail.valid,
                    rail.bus_voltage_v, rail.sampled_at_us, now_us)) {
                ESP_LOGE(TAG,
                    "AD7606 +5 V rail lost (INA3221 CH3 %.2f V < %.2f V): "
                    "stopping acquisition and releasing AD7606 pins to Hi-Z",
                    (double)rail.bus_voltage_v, (double)AD7606_RAIL_LOSS_V);
                ad7606_status_set_power(
                    AD7606_POWER_GATE_RAIL_LOW, true, rail.bus_voltage_v);
                ad7606_lifecycle_set_running(false);
                break;
            }
        }
#endif

        const int busy_before = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
        uint64_t sample_index = 0U;
        bool discard_candidate = false;
        const bool candidate_exists = ad7606_acquisition_sample_index_from_pcnt(
            s_read_mode,
            conversion_count,
            busy_before != 0,
            &sample_index,
            &discard_candidate
        );
        if (!candidate_exists) {
            runtime_observability_beat(RUNTIME_TASK_ACQUISITION, successful_samples);
            continue;
        }
        if (discard_candidate) {
            (void)ad7606_account_sample_index(sample_index, false);
            runtime_observability_beat(RUNTIME_TASK_ACQUISITION, successful_samples);
            continue;
        }

        ad7606_status_record_attempt();
        ad7606_sample_t sample = {0};
        err = ad7606_driver_read_result(&sample);
        const int busy_after = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
        uint64_t count_after = conversion_count;
        if (err == ESP_OK) {
            err = ad7606_read_conversion_count(&count_after);
        }

        bool stable = err == ESP_OK;
        if (s_read_mode == AD7606_READ_MODE_DURING_CONVERSION) {
            stable = stable && busy_before != 0 && busy_after != 0 &&
                count_after == conversion_count;
        }
        else {
            stable = stable && count_after == conversion_count;
        }

        ad7606_serial_observation_t serial_observation;
        if (stable) {
            err = ad7606_driver_get_last_serial_observation(&serial_observation);
            stable = err == ESP_OK && serial_observation.spi_transaction_ok;
        }
        if (stable) {
            serial_observation.busy_level = busy_after;
            err = ad7606_publish_sample(
                sample_index,
                &sample,
                &serial_observation,
                &successful_samples
            );
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Sample %" PRIu64 " not published: %s",
                    sample_index, esp_err_to_name(err));
            }
        }
        else {
            (void)ad7606_account_sample_index(sample_index, false);
            if (err != ESP_OK) {
                const uint32_t failures = ad7606_status_record_failure();
                if (failures == 1U || failures %
                    CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ == 0U) {
                    ESP_LOGW(TAG, "AD7606 result read failed: %s",
                        esp_err_to_name(err));
                }
            }
        }
        runtime_observability_beat(RUNTIME_TASK_ACQUISITION, successful_samples);
    }

    ad7606_worker_cleanup(
        ad7606_lifecycle_stop_requested(), &successful_samples);
    runtime_observability_set_health_phase(
        RUNTIME_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_IDLE
    );
    ad7606_lifecycle_finish_worker();
    ESP_LOGI(TAG, "Stopped");
    vTaskDelete(NULL);
}


esp_err_t ad7606_acquisition_start(void)
{
    if (!ad7606_lifecycle_begin_start()) {
        return ad7606_lifecycle_task() != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    ad7606_status_reset();
    ad7606_rail_watch_reset(&s_rail_watch);
    s_rail_last_check_conversion = 0U;
#if defined(CONFIG_SEISMIC_AD7606_RAIL_INTERLOCK)
    {
        /* MAX-245: an explicit start clears a latched over-current; if
         * the fault persists the monitor re-confirms it within ~2 s and
         * the worker stops again. */
        (void)ina3221_monitor_clear_overcurrent(INA3221_MONITOR_RAIL_AD7606_5V);

        /* MAX-242: decide BEFORE the Core 1 worker exists, so a refused
         * start never configures or drives a single AD7606 pin. */
        ina3221_monitor_rail_t rail;
        ad7606_read_rail(&rail);
        const ad7606_power_gate_t gate = ad7606_power_gate(
            rail.available, rail.valid, rail.bus_voltage_v,
            rail.sampled_at_us, esp_timer_get_time()
        );
        if (gate == AD7606_POWER_GATE_RAIL_LOW) {
            ESP_LOGE(
                TAG,
                "START refused: AD7606 +5 V rail %.2f V < %.2f V (INA3221 CH3); "
                "no AD7606 pin was driven. If the AD7606 supply is not routed "
                "through INA3221 CH3, disable CONFIG_SEISMIC_AD7606_RAIL_INTERLOCK.",
                (double)rail.bus_voltage_v,
                (double)AD7606_RAIL_START_MIN_V
            );
            ad7606_status_set_power(gate, true, rail.bus_voltage_v);
            ad7606_lifecycle_finish_start();
            return ESP_ERR_INVALID_STATE;
        }
        if (gate == AD7606_POWER_GATE_UNVERIFIED) {
            ESP_LOGW(
                TAG,
                "AD7606 +5 V rail not verified (no fresh INA3221 CH3 reading); "
                "starting on the hardware VIO interlock alone"
            );
        }
        ad7606_status_set_power(gate, false, rail.bus_voltage_v);
    }
#endif

    ad7606_start_context_t start_context = {
        .ready =
            xSemaphoreCreateBinary(),

        .result =
            ESP_ERR_INVALID_STATE,
    };

    if (start_context.ready == NULL) {
        ad7606_lifecycle_finish_start();
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t created_task = NULL;
    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            ad7606_worker,
            "ad7606_acq",
            APP_TASK_STACK_AD7606,
            &start_context,
            APP_TASK_PRIORITY_AD7606,
            &created_task,
            APP_TASK_CORE_REALTIME
        );

    if (task_result != pdPASS) {
        vSemaphoreDelete(
            start_context.ready
        );

        ad7606_lifecycle_finish_start();

        return ESP_ERR_NO_MEM;
    }
    (void)created_task;

    /*
     * Keep start() synchronous: success means SPI3, the ADC self-test
     * and the sampling timer all completed on Core 1.
     */
    (void)xSemaphoreTake(
        start_context.ready,
        portMAX_DELAY
    );

    const esp_err_t result =
        start_context.result;

    vSemaphoreDelete(
        start_context.ready
    );
    ad7606_lifecycle_finish_start();

    return result;
}


void ad7606_acquisition_stop(void)
{
    ad7606_lifecycle_request_stop();
    while (ad7606_lifecycle_task() != NULL || ad7606_lifecycle_starting()) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
    ad7606_status_set_running(false);
}


esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if defined(AD7606_ACQUISITION_HOST_TEST)
    ++s_test_counters.status_critical_entries;
#endif
    portENTER_CRITICAL(
        &s_status_lock
    );

    *status =
        s_status;

    portEXIT_CRITICAL(
        &s_status_lock
    );

    return ESP_OK;
}
