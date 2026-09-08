#include "ad7606_acquisition.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "ad7606_driver.h"
#include "ad7606_acquisition_timing.h"

#include "runtime_observability.h"
#include "app_task_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


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

    portEXIT_CRITICAL(
        &s_status_lock
    );
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
    const float latest_uv[AD7606_CHANNEL_COUNT]
)
{
    portENTER_CRITICAL(
        &s_status_lock
    );

    s_status.samples++;

    s_status.consecutive_failures =
        0;

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

    while (s_running) {
        (void)ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        if (!s_running) {
            break;
        }

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
            all_channels_uv
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
