#include "keypad.h"

#include <stdbool.h>
#include <stddef.h>
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

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG = "KEYPAD";

#define KEYPAD_BUTTON_COUNT 5U
#define KEYPAD_DEBOUNCE_MS 60U
#define KEYPAD_SAMPLE_COUNT 16U
#define KEYPAD_BUTTON_SAMPLE_COUNT 4U
#define KEYPAD_BUTTON_CACHE_WINDOW_US 1000LL

#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS

/*
 * The diagnostic profile deliberately keeps all accounting bounded and
 * local to the ESP_TIMER_TASK callback. A four-microsecond histogram gives a
 * useful p95 without sorting or allocating from the timer task. Values above
 * the last bucket are reported through overflow and the p95 is the bucket's
 * upper bound.
 */
#define KEYPAD_TIMING_REPORT_PERIOD_US 1000000LL
#define KEYPAD_TIMING_PROBE_PERIOD_US 10000LL
#define KEYPAD_TIMING_HISTOGRAM_WIDTH_US 4U
#define KEYPAD_TIMING_HISTOGRAM_BUCKETS 512U

#endif

/* Preserve the raw thresholds from the original KS0466 implementation for
 * both the compatibility read API and the custom Button driver below. */
#define KEYPAD_SW1_MAX_RAW 275
#define KEYPAD_SW2_MAX_RAW 905
#define KEYPAD_SW3_MAX_RAW 1605
#define KEYPAD_SW4_MAX_RAW 2411
#define KEYPAD_SW5_MAX_RAW 3484

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
#if defined(CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH) && \
    CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    return "ESP_ADC_BENCH";
#elif defined(CONFIG_SEISMIC_SOURCE_AD7606) && CONFIG_SEISMIC_SOURCE_AD7606
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


static void keypad_timing_record_duration(
    uint32_t duration_us,
    uint32_t *count,
    uint32_t *min_us,
    uint32_t *max_us,
    uint64_t *sum_us,
    uint32_t *overflow,
    uint32_t *histogram
)
{
    (*count)++;
    if (duration_us < *min_us) {
        *min_us = duration_us;
    }
    if (duration_us > *max_us) {
        *max_us = duration_us;
    }
    *sum_us += duration_us;

    uint32_t bucket = duration_us / KEYPAD_TIMING_HISTOGRAM_WIDTH_US;
    if (bucket >= KEYPAD_TIMING_HISTOGRAM_BUCKETS) {
        bucket = KEYPAD_TIMING_HISTOGRAM_BUCKETS - 1U;
        (*overflow)++;
    }
    histogram[bucket]++;
}


static uint32_t keypad_timing_percentile_us(
    uint32_t count,
    const uint32_t *histogram
)
{
    if (count == 0U) {
        return 0U;
    }

    /* ceil(count * 95 / 100), with the first sample as the minimum rank. */
    const uint32_t rank = (count * 95U + 99U) / 100U;
    uint32_t accumulated = 0U;
    for (uint32_t bucket = 0U;
         bucket < KEYPAD_TIMING_HISTOGRAM_BUCKETS;
         ++bucket) {
        accumulated += histogram[bucket];
        if (accumulated >= rank) {
            if (bucket == KEYPAD_TIMING_HISTOGRAM_BUCKETS - 1U) {
                return KEYPAD_TIMING_HISTOGRAM_BUCKETS *
                    KEYPAD_TIMING_HISTOGRAM_WIDTH_US;
            }
            return (bucket + 1U) * KEYPAD_TIMING_HISTOGRAM_WIDTH_US - 1U;
        }
    }

    return KEYPAD_TIMING_HISTOGRAM_BUCKETS *
        KEYPAD_TIMING_HISTOGRAM_WIDTH_US;
}


static void keypad_timing_log_window(int64_t now_us)
{
    const uint32_t window_ms = s_timing_report_started_us > 0 &&
            now_us > s_timing_report_started_us
        ? (uint32_t)((now_us - s_timing_report_started_us) / 1000LL)
        : 0U;

    if (s_adc_timing_snapshots > 0U) {
        ESP_LOGI(
            TAG,
            "KEYPAD_ADC_TIMING: source=%s window_ms=%u attempts=%u snapshots=%u busy=%u errors=%u min_us=%u avg_us=%u p95_us=%u max_us=%u resolution_us=%u overflow=%u",
            keypad_timing_source_name(),
            (unsigned)window_ms,
            (unsigned)s_adc_timing_attempts,
            (unsigned)s_adc_timing_snapshots,
            (unsigned)s_adc_timing_busy,
            (unsigned)s_adc_timing_errors,
            (unsigned)s_adc_timing_min_us,
            (unsigned)(s_adc_timing_sum_us / s_adc_timing_snapshots),
            (unsigned)keypad_timing_percentile_us(
                s_adc_timing_snapshots,
                s_adc_timing_histogram
            ),
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
        ESP_LOGI(
            TAG,
            "KEYPAD_TIMER_TIMING: period_us=%u window_ms=%u samples=%u min_lateness_us=%u avg_lateness_us=%u p95_lateness_us=%u max_lateness_us=%u late_count=%u overrun_count=%u resolution_us=%u overflow=%u",
            (unsigned)KEYPAD_TIMING_PROBE_PERIOD_US,
            (unsigned)window_ms,
            (unsigned)s_timer_timing_samples,
            (unsigned)s_timer_timing_min_us,
            (unsigned)(s_timer_timing_sum_us / s_timer_timing_samples),
            (unsigned)keypad_timing_percentile_us(
                s_timer_timing_samples,
                s_timer_timing_histogram
            ),
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


static keypad_key_t keypad_decode_raw(int raw)
{
    if (raw <= KEYPAD_SW1_MAX_RAW) {
        return KEYPAD_KEY_LEFT;
    }
    if (raw <= KEYPAD_SW2_MAX_RAW) {
        return KEYPAD_KEY_UP;
    }
    if (raw <= KEYPAD_SW3_MAX_RAW) {
        return KEYPAD_KEY_DOWN;
    }
    if (raw <= KEYPAD_SW4_MAX_RAW) {
        return KEYPAD_KEY_RIGHT;
    }
    if (raw <= KEYPAD_SW5_MAX_RAW) {
        return KEYPAD_KEY_OK;
    }
    return KEYPAD_KEY_NONE;
}


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
            return keypad_button_level_from_cache(button);
        }
        if (err != ESP_OK) {
#if defined(ESP_PLATFORM) && \
    defined(CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS) && \
    CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS
            s_adc_timing_errors++;
#endif
            s_adc_cache_valid = false;
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

        s_adc_cache_key = keypad_decode_raw(raw);
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


static void keypad_button_callback(void *button_handle, void *user_data)
{
    (void)button_handle;

    /* No delay, storage, or application lock is allowed in this callback. */
    (void)app_events_post(
        APP_EVENT_KEYPAD,
        (int32_t)(keypad_key_t)(uintptr_t)user_data
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
    s_initialized = false;
}
