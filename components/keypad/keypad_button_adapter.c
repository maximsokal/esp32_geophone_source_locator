#include "keypad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "adc_service.h"
#include "app_events.h"
#include "board_config.h"
#include "button_interface.h"
#include "iot_button.h"
#include "keypad_decode.h"
#include "keypad_timing_stats.h"

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG = "KEYPAD";

#define KEYPAD_BUTTON_COUNT 5U
#define KEYPAD_DEBOUNCE_MS 60U
#define KEYPAD_SAMPLE_COUNT 16U
#define KEYPAD_BUTTON_SAMPLE_COUNT KEYPAD_SAMPLE_COUNT
#define KEYPAD_BUTTON_CACHE_WINDOW_US 1000LL
#define KEYPAD_RELEASE_CONFIRM_SAMPLES 3U
#define KEYPAD_BOOT_INPUT_DIAGNOSTIC_PERIOD_MS 2000U
#define KEYPAD_LEVEL_LOG_MIN_DELTA_RAW 100U
#define KEYPAD_LEVEL_LOG_SETTLE_BAND_RAW 60U
#define KEYPAD_LEVEL_LOG_SETTLE_SAMPLES 3U
#define KEYPAD_IDLE_WARN_DELAY_MS 3000U
#define KEYPAD_IDLE_WARN_PERIOD_MS 5000U

#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS

/*
 * The diagnostic profile deliberately keeps all accounting bounded and
 * local to the ESP_TIMER_TASK callback. A four-microsecond histogram gives a
 * useful p95 without sorting or allocating from the timer task. Values above
 * the normal range are counted separately as right-censored overflow samples.
 */
#define KEYPAD_TIMING_REPORT_PERIOD_US 1000000LL
#define KEYPAD_TIMING_PROBE_PERIOD_US 10000LL

#endif

/* The official ADC Button driver is intentionally not used: it reads a
 * borrowed ADC1 handle directly from its esp_timer callback. */
static const keypad_key_t s_keys[KEYPAD_BUTTON_COUNT] = {
    KEYPAD_KEY_LEFT,
    KEYPAD_KEY_UP,
    KEYPAD_KEY_DOWN,
    KEYPAD_KEY_RIGHT,
    KEYPAD_KEY_OK,
};


typedef struct {
    button_driver_t base;
    keypad_key_t key;
} keypad_button_driver_t;


static button_handle_t s_buttons[KEYPAD_BUTTON_COUNT];
static keypad_button_driver_t s_button_drivers[KEYPAD_BUTTON_COUNT];
static adc_channel_t s_adc_channel;
static bool s_initialized;
static bool s_adc_cache_valid;
static int64_t s_adc_cache_time_us;
static bool s_adc_cache_attempt_valid;
static int64_t s_adc_cache_attempt_time_us;
static keypad_key_t s_adc_cache_key;
/* The release-gated key state also supplies the decoder's hysteresis state. */
static keypad_key_t s_trace_key = KEYPAD_KEY_NONE;
/* Last raw decoder output, including a first candidate blocked by the gate. */
static keypad_key_t s_trace_decoded_key = KEYPAD_KEY_NONE;
static bool s_release_armed;
static unsigned s_release_idle_samples;
static bool s_level_log_valid;
static int s_level_logged_raw;
static int s_level_run_min_raw;
static int s_level_run_max_raw;
static unsigned s_level_run_count;
static _Atomic(uint32_t) s_boot_input_last_log_ms;
static _Atomic(bool) s_boot_input_log_seen;
static _Atomic(uint32_t) s_idle_warning_monitor_start_ms;
static _Atomic(uint32_t) s_idle_warning_last_log_ms;
static _Atomic(bool) s_idle_warning_log_seen;
static _Atomic(bool) s_idle_warning_gate_armed_seen;
static _Atomic(keypad_event_callback_t) s_boot_event_callback = NULL;
static _Atomic(void *) s_boot_event_context = NULL;


static bool keypad_boot_monitor_active(void)
{
    return atomic_load_explicit(
        &s_boot_event_callback,
        memory_order_acquire
    ) != NULL;
}


static bool keypad_boot_input_log_due(int64_t now_us)
{
    if (!keypad_boot_monitor_active()) {
        return false;
    }

    if (!atomic_load_explicit(&s_boot_input_log_seen, memory_order_relaxed)) {
        return true;
    }

    const uint32_t now_ms = (uint32_t)(now_us / 1000LL);
    const uint32_t last_log_ms = atomic_load_explicit(
        &s_boot_input_last_log_ms,
        memory_order_relaxed
    );
    return (uint32_t)(now_ms - last_log_ms) >=
        KEYPAD_BOOT_INPUT_DIAGNOSTIC_PERIOD_MS;
}


static unsigned keypad_abs_diff_raw(int left, int right)
{
    return left >= right
        ? (unsigned)(left - right)
        : (unsigned)(right - left);
}


static bool keypad_level_log_update(int raw)
{
    if (!s_level_log_valid) {
        s_level_log_valid = true;
        s_level_logged_raw = raw;
        s_level_run_count = 0U;
        return false;
    }

    if (keypad_abs_diff_raw(raw, s_level_logged_raw) <
        KEYPAD_LEVEL_LOG_MIN_DELTA_RAW) {
        s_level_run_count = 0U;
        return false;
    }

    const int run_min = s_level_run_count == 0U || raw < s_level_run_min_raw
        ? raw
        : s_level_run_min_raw;
    const int run_max = s_level_run_count == 0U || raw > s_level_run_max_raw
        ? raw
        : s_level_run_max_raw;
    if (
        s_level_run_count == 0U ||
        keypad_abs_diff_raw(run_max, run_min) >
            KEYPAD_LEVEL_LOG_SETTLE_BAND_RAW
    ) {
        s_level_run_min_raw = raw;
        s_level_run_max_raw = raw;
        s_level_run_count = 1U;
        return false;
    }

    s_level_run_min_raw = run_min;
    s_level_run_max_raw = run_max;
    if (++s_level_run_count < KEYPAD_LEVEL_LOG_SETTLE_SAMPLES) {
        return false;
    }

    s_level_logged_raw = raw;
    s_level_run_count = 0U;
    return true;
}


static void keypad_level_log_reset(void)
{
    s_level_log_valid = false;
    s_level_logged_raw = 0;
    s_level_run_min_raw = 0;
    s_level_run_max_raw = 0;
    s_level_run_count = 0U;
}


static void keypad_idle_warning_reset(void)
{
    atomic_store_explicit(
        &s_idle_warning_monitor_start_ms,
        0U,
        memory_order_relaxed
    );
    atomic_store_explicit(&s_idle_warning_last_log_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_idle_warning_log_seen, false, memory_order_relaxed);
    atomic_store_explicit(
        &s_idle_warning_gate_armed_seen,
        false,
        memory_order_relaxed
    );
}


static void keypad_idle_warning_maybe_log(
    bool boot_monitor_active,
    int raw,
    int64_t now_us,
    bool release_armed_before_sample
)
{
    if (!boot_monitor_active) {
        return;
    }

    if (s_release_armed || release_armed_before_sample) {
        atomic_store_explicit(
            &s_idle_warning_gate_armed_seen,
            true,
            memory_order_relaxed
        );
        return;
    }
    if (atomic_load_explicit(
            &s_idle_warning_gate_armed_seen,
            memory_order_relaxed
        )) {
        return;
    }

    const uint32_t now_ms = (uint32_t)(now_us / 1000LL);
    const uint32_t start_ms = atomic_load_explicit(
        &s_idle_warning_monitor_start_ms,
        memory_order_relaxed
    );
    if ((uint32_t)(now_ms - start_ms) < KEYPAD_IDLE_WARN_DELAY_MS) {
        return;
    }

    if (atomic_load_explicit(&s_idle_warning_log_seen, memory_order_relaxed)) {
        const uint32_t last_log_ms = atomic_load_explicit(
            &s_idle_warning_last_log_ms,
            memory_order_relaxed
        );
        if ((uint32_t)(now_ms - last_log_ms) < KEYPAD_IDLE_WARN_PERIOD_MS) {
            return;
        }
    }

    ESP_LOGW(
        TAG,
        "KEYPAD_IDLE_LEVEL_INVALID raw=%d need>=%d: key line is not pulled high - check keypad module + (3V3), - (GND) and S (GPIO8) wiring; no key can be detected",
        raw,
        KEYPAD_RELEASE_MIN_RAW
    );
    atomic_store_explicit(&s_idle_warning_last_log_ms, now_ms, memory_order_relaxed);
    atomic_store_explicit(&s_idle_warning_log_seen, true, memory_order_relaxed);
}


static void keypad_boot_input_log_unavailable(
    int64_t now_us,
    esp_err_t err
)
{
    if (!keypad_boot_input_log_due(now_us)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "KEYPAD_BOOT_INPUT raw=NA decoded=%s release_armed=%u adc_error=%s",
        keypad_key_name(s_trace_decoded_key),
        s_release_armed ? 1U : 0U,
        esp_err_to_name(err)
    );
    atomic_store_explicit(
        &s_boot_input_last_log_ms,
        (uint32_t)(now_us / 1000LL),
        memory_order_relaxed
    );
    atomic_store_explicit(&s_boot_input_log_seen, true, memory_order_relaxed);
}


#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS

static esp_timer_handle_t s_timing_probe_timer;
static int64_t s_timing_probe_next_due_us;
static int64_t s_timing_report_started_us;

static uint32_t s_adc_timing_attempts;
static uint32_t s_adc_timing_snapshots;
static uint32_t s_adc_timing_busy;
static uint32_t s_adc_timing_errors;
static uint32_t s_adc_timing_min_us;
static uint32_t s_adc_timing_max_us;
static uint64_t s_adc_timing_sum_us;
static uint32_t s_adc_timing_overflow;
static uint32_t s_adc_timing_histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS];

static uint32_t s_timer_timing_samples;
static uint32_t s_timer_timing_late;
static uint32_t s_timer_timing_overrun;
static uint32_t s_timer_timing_min_us;
static uint32_t s_timer_timing_max_us;
static uint64_t s_timer_timing_sum_us;
static uint32_t s_timer_timing_overflow;
static uint32_t s_timer_timing_histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS];


static const char *keypad_timing_source_name(void)
{
#if defined(CONFIG_SEISMIC_SOURCE_AD7606) && CONFIG_SEISMIC_SOURCE_AD7606
    return "AD7606";
#else
    return "UNKNOWN";
#endif
}


static void keypad_timing_reset_window(void)
{
    s_adc_timing_attempts = 0U;
    s_adc_timing_snapshots = 0U;
    s_adc_timing_busy = 0U;
    s_adc_timing_errors = 0U;
    s_adc_timing_min_us = UINT32_MAX;
    s_adc_timing_max_us = 0U;
    s_adc_timing_sum_us = 0U;
    s_adc_timing_overflow = 0U;
    memset(s_adc_timing_histogram, 0, sizeof(s_adc_timing_histogram));

    s_timer_timing_samples = 0U;
    s_timer_timing_late = 0U;
    s_timer_timing_overrun = 0U;
    s_timer_timing_min_us = UINT32_MAX;
    s_timer_timing_max_us = 0U;
    s_timer_timing_sum_us = 0U;
    s_timer_timing_overflow = 0U;
    memset(s_timer_timing_histogram, 0, sizeof(s_timer_timing_histogram));
}


static void keypad_timing_log_window(int64_t now_us)
{
    const uint32_t window_ms = s_timing_report_started_us > 0 &&
            now_us > s_timing_report_started_us
        ? (uint32_t)((now_us - s_timing_report_started_us) / 1000LL)
        : 0U;

    if (s_adc_timing_snapshots > 0U) {
        const keypad_timing_percentile_t p95 = keypad_timing_percentile_us(
            s_adc_timing_snapshots,
            s_adc_timing_overflow,
            s_adc_timing_histogram
        );
        ESP_LOGI(
            TAG,
            "KEYPAD_ADC_TIMING: source=%s window_ms=%u attempts=%u snapshots=%u busy=%u errors=%u min_us=%u avg_us=%u p95_us=%u p95_overflow=%u p95_censored_above_us=%u max_us=%u resolution_us=%u overflow=%u",
            keypad_timing_source_name(),
            (unsigned)window_ms,
            (unsigned)s_adc_timing_attempts,
            (unsigned)s_adc_timing_snapshots,
            (unsigned)s_adc_timing_busy,
            (unsigned)s_adc_timing_errors,
            (unsigned)s_adc_timing_min_us,
            (unsigned)(s_adc_timing_sum_us / s_adc_timing_snapshots),
            (unsigned)p95.value_us,
            (unsigned)(p95.overflow ? 1U : 0U),
            (unsigned)keypad_timing_p95_censored_above_us(p95),
            (unsigned)s_adc_timing_max_us,
            (unsigned)KEYPAD_TIMING_HISTOGRAM_WIDTH_US,
            (unsigned)s_adc_timing_overflow
        );
    } else {
        ESP_LOGW(
            TAG,
            "KEYPAD_ADC_TIMING: source=%s window_ms=%u attempts=%u snapshots=0 busy=%u errors=%u",
            keypad_timing_source_name(),
            (unsigned)window_ms,
            (unsigned)s_adc_timing_attempts,
            (unsigned)s_adc_timing_busy,
            (unsigned)s_adc_timing_errors
        );
    }

    if (s_timer_timing_samples > 0U) {
        const keypad_timing_percentile_t p95 = keypad_timing_percentile_us(
            s_timer_timing_samples,
            s_timer_timing_overflow,
            s_timer_timing_histogram
        );
        ESP_LOGI(
            TAG,
            "KEYPAD_TIMER_TIMING: period_us=%u window_ms=%u samples=%u min_lateness_us=%u avg_lateness_us=%u p95_lateness_us=%u p95_overflow=%u p95_censored_above_us=%u max_lateness_us=%u late_count=%u overrun_count=%u resolution_us=%u overflow=%u",
            (unsigned)KEYPAD_TIMING_PROBE_PERIOD_US,
            (unsigned)window_ms,
            (unsigned)s_timer_timing_samples,
            (unsigned)s_timer_timing_min_us,
            (unsigned)(s_timer_timing_sum_us / s_timer_timing_samples),
            (unsigned)p95.value_us,
            (unsigned)(p95.overflow ? 1U : 0U),
            (unsigned)keypad_timing_p95_censored_above_us(p95),
            (unsigned)s_timer_timing_max_us,
            (unsigned)s_timer_timing_late,
            (unsigned)s_timer_timing_overrun,
            (unsigned)KEYPAD_TIMING_HISTOGRAM_WIDTH_US,
            (unsigned)s_timer_timing_overflow
        );
    } else {
        ESP_LOGW(
            TAG,
            "KEYPAD_TIMER_TIMING: period_us=%u window_ms=%u samples=0",
            (unsigned)KEYPAD_TIMING_PROBE_PERIOD_US,
            (unsigned)window_ms
        );
    }

    keypad_timing_reset_window();
    s_timing_report_started_us = now_us;
}


static void keypad_timing_probe_callback(void *argument)
{
    (void)argument;

    const int64_t now_us = esp_timer_get_time();
    if (s_timing_probe_next_due_us > 0) {
        const int64_t lateness_us = now_us > s_timing_probe_next_due_us
            ? now_us - s_timing_probe_next_due_us
            : 0LL;
        keypad_timing_record_duration(
            lateness_us > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)lateness_us,
            &s_timer_timing_samples,
            &s_timer_timing_min_us,
            &s_timer_timing_max_us,
            &s_timer_timing_sum_us,
            &s_timer_timing_overflow,
            s_timer_timing_histogram
        );
        if (lateness_us > 0) {
            s_timer_timing_late++;
        }
        if (lateness_us >= KEYPAD_TIMING_PROBE_PERIOD_US) {
            s_timer_timing_overrun++;
        }
    }

    s_timing_probe_next_due_us += KEYPAD_TIMING_PROBE_PERIOD_US;
    if (s_timing_probe_next_due_us < now_us -
            (KEYPAD_TIMING_PROBE_PERIOD_US * 16LL)) {
        /* Do not create a burst of immediately-due callbacks after a long
         * sleep or a reset of the timer service. Start a fresh schedule. */
        s_timing_probe_next_due_us = now_us +
            KEYPAD_TIMING_PROBE_PERIOD_US;
    }

    if (s_timing_report_started_us > 0 &&
        now_us - s_timing_report_started_us >=
            KEYPAD_TIMING_REPORT_PERIOD_US) {
        keypad_timing_log_window(now_us);
    }
}


static esp_err_t keypad_timing_start(void)
{
    const esp_timer_create_args_t timer_config = {
        .callback = keypad_timing_probe_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "keypad_timing_probe",
    };
    esp_err_t err = esp_timer_create(&timer_config, &s_timing_probe_timer);
    if (err != ESP_OK) {
        return err;
    }

    const int64_t now_us = esp_timer_get_time();
    keypad_timing_reset_window();
    s_timing_report_started_us = now_us;
    s_timing_probe_next_due_us = now_us + KEYPAD_TIMING_PROBE_PERIOD_US;

    err = esp_timer_start_periodic(
        s_timing_probe_timer,
        KEYPAD_TIMING_PROBE_PERIOD_US
    );
    if (err != ESP_OK) {
        (void)esp_timer_delete(s_timing_probe_timer);
        s_timing_probe_timer = NULL;
        s_timing_probe_next_due_us = 0;
        s_timing_report_started_us = 0;
        return err;
    }

    ESP_LOGI(
        TAG,
        "KEYPAD_TIMING_READY source=%s button_period_ms=%u probe_period_us=%u resolution_us=%u",
        keypad_timing_source_name(),
        (unsigned)CONFIG_BUTTON_PERIOD_TIME_MS,
        (unsigned)KEYPAD_TIMING_PROBE_PERIOD_US,
        (unsigned)KEYPAD_TIMING_HISTOGRAM_WIDTH_US
    );
    return ESP_OK;
}


static void keypad_timing_stop(void)
{
    if (s_timing_probe_timer != NULL) {
        (void)esp_timer_stop(s_timing_probe_timer);
        (void)esp_timer_delete(s_timing_probe_timer);
        s_timing_probe_timer = NULL;
    }
    s_timing_probe_next_due_us = 0;
    s_timing_report_started_us = 0;
    keypad_timing_reset_window();
}

#endif


static keypad_button_driver_t *keypad_button_driver_from_base(
    button_driver_t *driver
)
{
    return (keypad_button_driver_t *)(
        (uint8_t *)driver - offsetof(keypad_button_driver_t, base)
    );
}


static uint8_t keypad_button_level_from_cache(
    const keypad_button_driver_t *button
)
{
    return s_adc_cache_valid && s_adc_cache_key == button->key
        ? BUTTON_ACTIVE
        : BUTTON_INACTIVE;
}


static uint8_t keypad_button_get_key_level(button_driver_t *driver)
{
    keypad_button_driver_t *button =
        keypad_button_driver_from_base(driver);
    const int64_t now_us = esp_timer_get_time();

    /* Button core polls all five handles in one timer callback. Share one
     * broker-owned sample across that callback so each handle sees the same
     * conversion without multiplying ADC traffic. The short window is below
     * the 10 ms button period and is never used to bypass adc_service. */
    if (
        !s_adc_cache_valid ||
        now_us - s_adc_cache_time_us > KEYPAD_BUTTON_CACHE_WINDOW_US
    ) {
        /* A failed non-blocking attempt must not be retried by the next
         * logical Button object in the same timer callback. Keep the last
         * state for this cache window and let the next window try again. */
        if (
            s_adc_cache_attempt_valid &&
            now_us - s_adc_cache_attempt_time_us <=
                KEYPAD_BUTTON_CACHE_WINDOW_US
        ) {
            return keypad_button_level_from_cache(button);
        }

        s_adc_cache_attempt_valid = true;
        s_adc_cache_attempt_time_us = now_us;

        int raw = 0;
#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
        s_adc_timing_attempts++;
        const int64_t adc_started_us = esp_timer_get_time();
#endif
        esp_err_t err = adc_service_try_read_average_raw(
            s_adc_channel,
            KEYPAD_BUTTON_SAMPLE_COUNT,
            &raw
        );
#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
        const int64_t adc_finished_us = esp_timer_get_time();
#endif
        if (err == ESP_ERR_TIMEOUT) {
#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
            s_adc_timing_busy++;
#endif
            keypad_boot_input_log_unavailable(now_us, err);
            return keypad_button_level_from_cache(button);
        }
        if (err != ESP_OK) {
#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
            s_adc_timing_errors++;
#endif
            s_adc_cache_valid = false;
            keypad_boot_input_log_unavailable(now_us, err);
            return keypad_button_level_from_cache(button);
        }

#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
        const int64_t adc_duration_us = adc_finished_us - adc_started_us;
        keypad_timing_record_duration(
            adc_duration_us > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)(adc_duration_us > 0 ? adc_duration_us : 0),
            &s_adc_timing_snapshots,
            &s_adc_timing_min_us,
            &s_adc_timing_max_us,
            &s_adc_timing_sum_us,
            &s_adc_timing_overflow,
            s_adc_timing_histogram
        );
#endif

        const bool release_armed_before_sample = s_release_armed;
        const keypad_key_t raw_decoded_key = keypad_decode_raw_with_previous(
            raw,
            s_trace_key
        );
        keypad_key_t decoded_key = raw_decoded_key;

        if (
            raw >= KEYPAD_RELEASE_MIN_RAW &&
            raw <= 4095 &&
            decoded_key == KEYPAD_KEY_NONE
        ) {
            if (s_release_idle_samples < KEYPAD_RELEASE_CONFIRM_SAMPLES) {
                ++s_release_idle_samples;
            }
            if (s_release_idle_samples >= KEYPAD_RELEASE_CONFIRM_SAMPLES) {
                s_release_armed = true;
            }
        } else {
            s_release_idle_samples = 0U;
        }

        /* Button objects start polling before the interactive boundary. Do
         * not let a floating input that happens to land in a calibrated key
         * range create the first logical press; three consecutive high-idle
         * samples above every key window must be observed first. Once a press
         * is admitted, normal hysteresis and Button debounce own that press. */
        if (
            s_trace_key == KEYPAD_KEY_NONE &&
            decoded_key != KEYPAD_KEY_NONE &&
            !s_release_armed
        ) {
            decoded_key = KEYPAD_KEY_NONE;
        }
        if (
            s_trace_key == KEYPAD_KEY_NONE &&
            decoded_key != KEYPAD_KEY_NONE
        ) {
            s_release_armed = false;
            s_release_idle_samples = 0U;
        }
        const int64_t diagnostic_time_us = esp_timer_get_time();
        const bool boot_monitor_active = keypad_boot_monitor_active();
        const int previous_logged_raw = s_level_logged_raw;
        const bool settled_level_changed = keypad_level_log_update(raw);
        s_trace_decoded_key = raw_decoded_key;
        keypad_idle_warning_maybe_log(
            boot_monitor_active,
            raw,
            diagnostic_time_us,
            release_armed_before_sample
        );
        if (settled_level_changed) {
            /* The timestamp marks the completed three-sample settle run. */
            ESP_LOGI(
                TAG,
                "KEYPAD_RAW_TRANSITION raw=%d from=%s to=%s decoded=%s release_armed=%u",
                raw,
                keypad_key_name(keypad_decode_raw(previous_logged_raw)),
                keypad_key_name(keypad_decode_raw(raw)),
                keypad_key_name(raw_decoded_key),
                s_release_armed ? 1U : 0U
            );
            if (boot_monitor_active) {
                atomic_store_explicit(
                    &s_boot_input_last_log_ms,
                    (uint32_t)(diagnostic_time_us / 1000LL),
                    memory_order_relaxed
                );
                atomic_store_explicit(
                    &s_boot_input_log_seen,
                    true,
                    memory_order_relaxed
                );
            }
        }
        if (
            !settled_level_changed &&
            boot_monitor_active &&
            keypad_boot_input_log_due(diagnostic_time_us)
        ) {
            ESP_LOGI(
                TAG,
                "KEYPAD_BOOT_INPUT raw=%d decoded=%s release_armed=%u",
                raw,
                keypad_key_name(raw_decoded_key),
                s_release_armed ? 1U : 0U
            );
            atomic_store_explicit(
                &s_boot_input_last_log_ms,
                (uint32_t)(diagnostic_time_us / 1000LL),
                memory_order_relaxed
            );
            atomic_store_explicit(
                &s_boot_input_log_seen,
                true,
                memory_order_relaxed
            );
        }

        s_trace_key = decoded_key;
        s_adc_cache_key = decoded_key;
        /* Timestamp the completed broker read, not the potentially stale
         * pre-read time. This keeps all five callbacks in this timer cycle on
         * the same sample even when a conversion itself takes time. */
        s_adc_cache_time_us = esp_timer_get_time();
        s_adc_cache_valid = true;
        s_adc_cache_attempt_valid = false;
    }

    return keypad_button_level_from_cache(button);
}


static esp_err_t keypad_button_driver_delete(button_driver_t *driver)
{
    /* Driver storage is static because iot_button_create() retains the
     * caller-owned driver pointer until iot_button_delete(). */
    (void)driver;
    return ESP_OK;
}


void keypad_button_adapter_stop(void);
void keypad_button_adapter_stop_boot(void);


static void keypad_button_callback(void *button_handle, void *user_data)
{
    (void)button_handle;

    const keypad_key_t key =
        (keypad_key_t)(uintptr_t)user_data;

    ESP_LOGI(
        TAG,
        "KEYPAD_BUTTON_PRESS key=%s value=%d",
        keypad_key_name(key),
        (int)key
    );

    keypad_event_callback_t boot_callback =
        atomic_load_explicit(&s_boot_event_callback, memory_order_acquire);
    if (boot_callback != NULL) {
        void *boot_context =
            atomic_load_explicit(&s_boot_event_context, memory_order_relaxed);
        boot_callback(key, boot_context);
        return;
    }

    /* No delay, storage, or application lock is allowed in this callback. */
    (void)app_events_post(
        APP_EVENT_KEYPAD,
        (int32_t)key
    );
}


esp_err_t keypad_button_adapter_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = adc_service_configure_gpio(
        BOARD_KEYPAD_GPIO,
        ADC_ATTEN_DB_12,
        ADC_BITWIDTH_DEFAULT,
        &s_adc_channel
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure KS0466 ADC: %s", esp_err_to_name(err));
        return err;
    }

    s_adc_cache_valid = false;
    s_adc_cache_attempt_valid = false;
    s_trace_key = KEYPAD_KEY_NONE;
    s_trace_decoded_key = KEYPAD_KEY_NONE;
    s_release_armed = false;
    s_release_idle_samples = 0U;
    keypad_level_log_reset();
    keypad_idle_warning_reset();
    atomic_store_explicit(&s_boot_input_last_log_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_boot_input_log_seen, false, memory_order_relaxed);

#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
    err = keypad_timing_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start keypad timing probe: %s", esp_err_to_name(err));
        keypad_button_adapter_stop();
        return err;
    }
#endif

    for (size_t i = 0; i < KEYPAD_BUTTON_COUNT; ++i) {
        const button_config_t button_config = {
            .short_press_time = KEYPAD_DEBOUNCE_MS,
            .long_press_time = 1500U,
        };
        s_button_drivers[i] = (keypad_button_driver_t) {
            .base = {
                .enable_power_save = false,
                .get_key_level = keypad_button_get_key_level,
                .enter_power_save = NULL,
                .exit_power_save = NULL,
                .get_gpio_num = NULL,
                .del = keypad_button_driver_delete,
            },
            .key = s_keys[i],
        };

        err = iot_button_create(
            &button_config,
            &s_button_drivers[i].base,
            &s_buttons[i]
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create %s button: %s",
                keypad_key_name(s_keys[i]), esp_err_to_name(err));
            keypad_button_adapter_stop();
            return err;
        }

        /* PRESS_DOWN fires once for every debounced press, including each
         * subsequent press after release: this is the old monitor's event and
         * repeat/press behavior without an app-owned polling task. */
        err = iot_button_register_cb(
            s_buttons[i],
            BUTTON_PRESS_DOWN,
            NULL,
            keypad_button_callback,
            (void *)(uintptr_t)s_keys[i]
        );
        if (err != ESP_OK) {
            keypad_button_adapter_stop();
            return err;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "KS0466 Button objects ready: GPIO%d ADC1 channel %d",
        BOARD_KEYPAD_GPIO, (int)s_adc_channel);
    return ESP_OK;
}


esp_err_t keypad_button_adapter_start(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Retain the existing startup API; Button callbacks post directly to the
     * application event abstraction so this adapter cannot duplicate events. */
    (void)callback;
    (void)user_context;
    return ESP_OK;
}


esp_err_t keypad_button_adapter_read_key(keypad_key_t *key)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t err = adc_service_read_average_raw(
        s_adc_channel,
        KEYPAD_SAMPLE_COUNT,
        &raw
    );
    if (err != ESP_OK) {
        return err;
    }

    *key = keypad_decode_raw(raw);
    return ESP_OK;
}


void keypad_button_adapter_stop(void)
{
    keypad_button_adapter_stop_boot();

#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
    keypad_timing_stop();
#endif

    for (size_t i = 0; i < KEYPAD_BUTTON_COUNT; ++i) {
        if (s_buttons[i] != NULL) {
            (void)iot_button_delete(s_buttons[i]);
            s_buttons[i] = NULL;
        }
    }

    s_adc_cache_valid = false;
    s_adc_cache_attempt_valid = false;
    s_trace_key = KEYPAD_KEY_NONE;
    s_trace_decoded_key = KEYPAD_KEY_NONE;
    s_release_armed = false;
    s_release_idle_samples = 0U;
    keypad_level_log_reset();
    keypad_idle_warning_reset();
    atomic_store_explicit(&s_boot_input_last_log_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_boot_input_log_seen, false, memory_order_relaxed);
    s_initialized = false;
}


esp_err_t keypad_button_adapter_start_boot(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    atomic_store_explicit(
        &s_boot_event_context,
        user_context,
        memory_order_relaxed
    );
    keypad_idle_warning_reset();
    atomic_store_explicit(
        &s_idle_warning_monitor_start_ms,
        (uint32_t)(esp_timer_get_time() / 1000LL),
        memory_order_relaxed
    );
    atomic_store_explicit(
        &s_boot_event_callback,
        callback,
        memory_order_release
    );
    atomic_store_explicit(&s_boot_input_last_log_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_boot_input_log_seen, false, memory_order_relaxed);
    return ESP_OK;
}


void keypad_button_adapter_stop_boot(void)
{
    atomic_store_explicit(
        &s_boot_event_callback,
        NULL,
        memory_order_release
    );
    atomic_store_explicit(&s_boot_input_last_log_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_boot_input_log_seen, false, memory_order_relaxed);
    keypad_idle_warning_reset();
}
