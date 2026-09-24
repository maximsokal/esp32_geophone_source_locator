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
#include "ad7606_power_policy.h"
#include "ina3221_monitor.h"

#include "runtime_observability.h"
#include "app_task_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if SEISMIC_CHANNEL_COUNT != AD7606_SIGNAL_CHANNEL_COUNT
#error "AD7606 signal quality scope must match the application seismic channel count"
#endif


static const char *TAG =
    "AD7606_ACQ";

static const char *DIAG_TAG =
    "AD7606_DIAG";


static TaskHandle_t s_task =
    NULL;

static esp_timer_handle_t s_timer =
    NULL;

static volatile bool s_running =
    false;

static portMUX_TYPE s_status_lock =
    portMUX_INITIALIZER_UNLOCKED;

static ad7606_acquisition_status_t s_status;

/* MAX-242: rail watch state, owned by the Core 1 worker. */
static ad7606_rail_watch_t s_rail_watch;
static int64_t s_rail_last_check_us = 0;
#define AD7606_RAIL_CHECK_PERIOD_US 1000000LL


#define AD7606_SERIAL_DIAG_LOG_INTERVAL_US  1000000LL

static ad7606_serial_diag_state_t s_serial_diag;
static int64_t s_serial_diag_last_log_us = 0;
static bool s_serial_diag_log_pending = false;


typedef struct
{
    SemaphoreHandle_t ready;
    esp_err_t result;

} ad7606_start_context_t;


typedef struct
{
    uint32_t count;
    int16_t min_raw;
    int16_t max_raw;
    int64_t sum_raw;
    double sum_sq_raw;
    uint32_t saturation_count;

} ad7606_diag_channel_t;


static ad7606_diag_channel_t s_diag[
    /* Diagnostics cover every ADC word; the app buffer remains four-channel. */
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
    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        ad7606_diag_channel_t *diag = &s_diag[channel];
        const int16_t raw = sample->raw[channel];

        if (raw < diag->min_raw) {
            diag->min_raw = raw;
        }

        if (raw > diag->max_raw) {
            diag->max_raw = raw;
        }

        diag->count++;
        diag->sum_raw += raw;
        diag->sum_sq_raw += (double)raw * (double)raw;

        if (raw == INT16_MIN || raw == INT16_MAX) {
            diag->saturation_count++;
        }
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


static void ad7606_diag_log_and_reset(void)
{
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
            ESP_LOGW(
                DIAG_TAG,
                "CH%u no samples",
                (unsigned)(channel + 1U)
            );
            continue;
        }

        const double count = (double)diag->count;
        const double mean_raw = (double)diag->sum_raw / count;

        double variance_raw =
            (diag->sum_sq_raw / count) -
            (mean_raw * mean_raw);

        /* Protect sqrt() from a tiny negative caused by rounding. */
        if (variance_raw < 0.0) {
            variance_raw = 0.0;
        }

        const double rms_raw = sqrt(variance_raw);
        const double p2p_raw =
            (double)diag->max_raw -
            (double)diag->min_raw;

        const double max_from_mean =
            fabs((double)diag->max_raw - mean_raw);
        const double min_from_mean =
            fabs((double)diag->min_raw - mean_raw);
        const double peak_raw =
            max_from_mean > min_from_mean
                ? max_from_mean
                : min_from_mean;

        ESP_LOGI(
            DIAG_TAG,
            "CH%u n=%u mean=%+.3fmV rms=%.3fmV p2p=%.3fmV peak=%.3fmV min=%+.3fmV max=%+.3fmV sat=%u %s",
            (unsigned)(channel + 1U),
            (unsigned)diag->count,
            mean_raw * lsb_mv,
            rms_raw * lsb_mv,
            p2p_raw * lsb_mv,
            peak_raw * lsb_mv,
            (double)diag->min_raw * lsb_mv,
            (double)diag->max_raw * lsb_mv,
            (unsigned)diag->saturation_count,
            ad7606_diag_state(diag)
        );
    }

    ad7606_diag_reset();
}


static void ad7606_status_reset(void)
{
    portENTER_CRITICAL(
        &s_status_lock
    );

    memset(
        &s_status,
        0,
        sizeof(s_status)
    );

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
    portENTER_CRITICAL(
        &s_status_lock
    );

    s_status.running =
        running;

    portEXIT_CRITICAL(
        &s_status_lock
    );
}


static void ad7606_status_record_timer_wake(
    uint32_t notification_count
)
{
    if (notification_count == 0U) {
        return;
    }

    portENTER_CRITICAL(
        &s_status_lock
    );

    s_status.timer_ticks =
        ad7606_acquisition_saturating_add_u64(
            s_status.timer_ticks,
            (uint64_t)notification_count
        );

    s_status.conversion_attempts =
        ad7606_acquisition_saturating_add_u64(
            s_status.conversion_attempts,
            1U
        );

    s_status.collapsed_periods =
        ad7606_acquisition_saturating_add_u64(
            s_status.collapsed_periods,
            ad7606_acquisition_collapsed_periods(notification_count)
        );

    portEXIT_CRITICAL(
        &s_status_lock
    );
}


static uint32_t ad7606_status_record_failure(void)
{
    uint32_t consecutive_failures;

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
    int busy_level
)
{
    portENTER_CRITICAL(
        &s_status_lock
    );

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
)
{
    const int64_t now_us = esp_timer_get_time();
    const bool periodic_due =
        serial_diag->suspicious_streak > 0U &&
        (
            s_serial_diag_last_log_us == 0 ||
            now_us - s_serial_diag_last_log_us >=
                AD7606_SERIAL_DIAG_LOG_INTERVAL_US
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
        s_serial_diag_log_pending = true;
    }

    if (!s_serial_diag_log_pending) {
        return;
    }

    if (
        s_serial_diag_last_log_us != 0 &&
        now_us - s_serial_diag_last_log_us <
            AD7606_SERIAL_DIAG_LOG_INTERVAL_US
    ) {
        return;
    }

    int16_t words[AD7606_SERIAL_WORD_COUNT] = {0};
    (void)ad7606_serial_decode_frame(
        observation->rx,
        words
    );

    ESP_LOGI(
        DIAG_TAG,
        "AD7606_SERIAL_DIAG: rx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X words=[%d,%d,%d,%d,%d,%d,%d,%d] class=%s transport_quality=%s signal_quality=%s stream_state=%s stream_stuck=%s busy=%d samples=%" PRIu64 " streak=%u repeated_streak=%u all_zero=%u all_ff=%u identical=%u varied=%u digital_interference=%u signal_rail_hits=%u signal_clips=%u window=%u transport_bad=%u all_zero=%u all_ff=%u identical=%u repeated=%u digital=%u signal_rail=%u signal_clip=%u serial_suspicious=%s",
        observation->rx[0],
        observation->rx[1],
        observation->rx[2],
        observation->rx[3],
        observation->rx[4],
        observation->rx[5],
        observation->rx[6],
        observation->rx[7],
        observation->rx[8],
        observation->rx[9],
        observation->rx[10],
        observation->rx[11],
        observation->rx[12],
        observation->rx[13],
        observation->rx[14],
        observation->rx[15],
        words[0],
        words[1],
        words[2],
        words[3],
        words[4],
        words[5],
        words[6],
        words[7],
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

    s_serial_diag_last_log_us = now_us;
    s_serial_diag_log_pending = false;
}


static void ad7606_timer_callback(
    void *argument
)
{
    (void)argument;

    if (
        s_running &&
        s_task != NULL
    ) {
        xTaskNotifyGive(
            s_task
        );
    }
}


static void ad7606_worker_cleanup(void)
{
    if (s_timer != NULL) {
        (void)esp_timer_stop(
            s_timer
        );

        (void)esp_timer_delete(
            s_timer
        );

        s_timer =
            NULL;
    }

    /* SPI3 was initialized by this Core 1 worker and is released here. */
    ad7606_driver_deinit();

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running =
        false;

    ad7606_status_set_running(
        false
    );
}


static esp_err_t ad7606_worker_initialize(void)
{
    /*
     * This executes on Core 1, so SPI3 initialization and its ISR
     * affinity stay on the real-time side of the application.
     */
    esp_err_t err =
        ad7606_driver_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        seismic_buffer_configure(
            SEISMIC_SOURCE_AD7606,
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
        );

    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_config = {
        .callback =
            ad7606_timer_callback,

        .arg =
            NULL,

        .dispatch_method =
            ESP_TIMER_TASK,

        .name =
            "ad7606_tick",
    };

    err =
        esp_timer_create(
            &timer_config,
            &s_timer
        );

    if (err != ESP_OK) {
        return err;
    }

    const uint64_t period_us =
        ad7606_acquisition_period_us(
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
        );

    s_running =
        true;

    err =
        esp_timer_start_periodic(
            s_timer,
            period_us
        );

    if (err != ESP_OK) {
        s_running =
            false;

        return err;
    }

    ad7606_status_set_running(
        true
    );

    ESP_LOGI(
        TAG,
        "Started: %d Hz, 8 channels, SPI3 dedicated, core=%d",
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        APP_TASK_CORE_REALTIME
    );

    return ESP_OK;
}


static void ad7606_worker(
    void *argument
)
{
    ad7606_start_context_t *start_context =
        (ad7606_start_context_t *)argument;

    s_task =
        xTaskGetCurrentTaskHandle();

    esp_err_t startup_err =
        ad7606_worker_initialize();

    if (startup_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Core 1 acquisition initialization failed: %s",
            esp_err_to_name(startup_err)
        );

        ad7606_worker_cleanup();

        s_task =
            NULL;

        start_context->result =
            startup_err;

        xSemaphoreGive(
            start_context->ready
        );

        vTaskDelete(
            NULL
        );

        return;
    }

    start_context->result =
        ESP_OK;

    xSemaphoreGive(
        start_context->ready
    );

    /* The caller owns this stack object after the semaphore is given. */
    start_context =
        NULL;

    uint32_t successful_samples =
        0;

    ad7606_diag_reset();
    ad7606_serial_diag_reset(
        &s_serial_diag
    );
    s_serial_diag_last_log_us = 0;
    s_serial_diag_log_pending = false;

    while (s_running) {
        const uint32_t notification_count =
            ulTaskNotifyTake(
                pdTRUE,
                portMAX_DELAY
            );

        if (!s_running) {
            /* STOP uses the same task notification but is not a timer period. */
            break;
        }

        ad7606_status_record_timer_wake(
            notification_count
        );

#if defined(CONFIG_SEISMIC_AD7606_RAIL_INTERLOCK)
        {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - s_rail_last_check_us >= AD7606_RAIL_CHECK_PERIOD_US) {
                s_rail_last_check_us = now_us;
                ina3221_monitor_rail_t rail;
                ad7606_read_rail(&rail);
                if (rail.overcurrent_latched) {
                    /* MAX-245: the monitor confirmed a critical current on
                     * this rail (two samples). Same reaction as rail loss:
                     * software protection only, ~1-3 s -- the supply's own
                     * current limit is the fast protection. */
                    ESP_LOGE(
                        TAG,
                        "AD7606 +5 V rail over-current (INA3221 CH3 %.3f A): "
                        "stopping acquisition and releasing AD7606 pins to Hi-Z",
                        (double)rail.overcurrent_latched_current_a
                    );
                    ad7606_status_set_overcurrent(
                        rail.bus_voltage_v, rail.overcurrent_latched_current_a
                    );
                    s_running = false;
                    break;
                }
                if (ad7606_rail_watch_update(
                        &s_rail_watch, rail.available, rail.valid,
                        rail.bus_voltage_v, rail.sampled_at_us, now_us)) {
                    ESP_LOGE(
                        TAG,
                        "AD7606 +5 V rail lost (INA3221 CH3 %.2f V < %.2f V): "
                        "stopping acquisition and releasing AD7606 pins to Hi-Z",
                        (double)rail.bus_voltage_v,
                        (double)AD7606_RAIL_LOSS_V
                    );
                    ad7606_status_set_power(
                        AD7606_POWER_GATE_RAIL_LOW, true, rail.bus_voltage_v
                    );
                    /* Cleanup below deinitializes the driver, which enters
                     * the Hi-Z safe state. */
                    s_running = false;
                    break;
                }
            }
        }
#endif

        ad7606_sample_t sample = {0};

        esp_err_t err =
            ad7606_driver_read(
                &sample
            );

        if (err != ESP_OK) {
            const uint32_t consecutive_failures =
                ad7606_status_record_failure();

            if (
                consecutive_failures == 1 ||
                (
                    consecutive_failures %
                    CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
                ) == 0
            ) {
                ESP_LOGW(
                    TAG,
                    "Read failed: %s (check BUSY/CONVST/SPI3 wiring)",
                    esp_err_to_name(err)
                );
            }

            /* Beat only after the potentially blocking read has returned. */
            runtime_observability_beat(
                RUNTIME_TASK_ACQUISITION,
                successful_samples
            );

            continue;
        }

        ad7606_serial_observation_t serial_observation;
        if (
            ad7606_driver_get_last_serial_observation(
                &serial_observation
            ) != ESP_OK ||
            !serial_observation.spi_transaction_ok
        ) {
            ESP_LOGW(
                DIAG_TAG,
                "SPI transaction returned OK without a serial observation"
            );
            continue;
        }

        ad7606_serial_diag_event_t serial_event;
        (void)ad7606_serial_diag_observe(
            &s_serial_diag,
            serial_observation.rx,
            &serial_event
        );

        float all_channels_uv[
            AD7606_CHANNEL_COUNT
        ];

        for (
            size_t channel = 0;
            channel < AD7606_CHANNEL_COUNT;
            ++channel
        ) {
            all_channels_uv[channel] =
                ad7606_raw_to_uv(
                    sample.raw[channel]
                );
        }

        ad7606_status_record_sample(
            &sample,
            all_channels_uv,
            &s_serial_diag,
            serial_observation.busy_level
        );

        ad7606_serial_diag_log_frame(
            &serial_observation,
            &s_serial_diag,
            &serial_event,
            (uint64_t)successful_samples + 1U
        );

        ad7606_diag_record(&sample);

        float seismic_channels[
            SEISMIC_CHANNEL_COUNT
        ];

        for (
            size_t channel = 0;
            channel < SEISMIC_CHANNEL_COUNT;
            ++channel
        ) {
            seismic_channels[channel] =
                all_channels_uv[channel];
        }

        /* Timestamp belongs to the simultaneous CONVST event. */
        err =
            seismic_buffer_push(
                sample.timestamp_us,
                seismic_channels
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Seismic buffer push failed: %s",
                esp_err_to_name(err)
            );
        }

        successful_samples++;

        runtime_observability_beat(
            RUNTIME_TASK_ACQUISITION,
            successful_samples
        );

        if (
            successful_samples %
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ ==
            0
        ) {
            ad7606_diag_log_and_reset();
        }
    }

    ad7606_worker_cleanup();

    s_task =
        NULL;

    ESP_LOGI(
        TAG,
        "Stopped"
    );

    vTaskDelete(
        NULL
    );
}


esp_err_t ad7606_acquisition_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    ad7606_status_reset();
    ad7606_rail_watch_reset(&s_rail_watch);
    s_rail_last_check_us = 0;

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
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            ad7606_worker,
            "ad7606_acq",
            APP_TASK_STACK_AD7606,
            &start_context,
            APP_TASK_PRIORITY_AD7606,
            &s_task,
            APP_TASK_CORE_REALTIME
        );

    if (task_result != pdPASS) {
        vSemaphoreDelete(
            start_context.ready
        );

        s_task =
            NULL;

        return ESP_ERR_NO_MEM;
    }

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

    return result;
}


void ad7606_acquisition_stop(void)
{
    TaskHandle_t task =
        s_task;

    if (task == NULL) {
        ad7606_status_set_running(
            false
        );

        return;
    }

    s_running =
        false;

    xTaskNotifyGive(
        task
    );

    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
}


esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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
