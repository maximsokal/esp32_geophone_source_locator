#include "seismic_acquisition.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "app_task_config.h"
#include "device_status.h"
#include "geophone_adc_test.h"
#include "seismic_buffer.h"
#include "seismic_test_generator.h"

#include "esp_log.h"


static const char *TAG =
    "SEISMIC_ACQ";


typedef struct
{
    const char *name;
    const char *transport_name;

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    esp_err_t (*start)(void);
    void (*stop)(void);

} seismic_source_driver_t;


#if CONFIG_SEISMIC_SOURCE_TEST

static const seismic_source_driver_t s_driver = {
    .name =
        "TEST",

    .transport_name =
        "SOFTWARE",

    .sample_rate_hz =
        SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ,

    .channel_count =
        SEISMIC_CHANNEL_COUNT,

    .start =
        seismic_test_generator_start,

    .stop =
        seismic_test_generator_stop,
};

#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH

static const seismic_source_driver_t s_driver = {
    .name =
        "ESP_ADC_BENCH",

    .transport_name =
        "ADC1",

    .sample_rate_hz =
        CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ,

    .channel_count =
        SEISMIC_CHANNEL_COUNT,

    .start =
        geophone_adc_test_start,

    .stop =
        geophone_adc_test_stop,
};

#elif CONFIG_SEISMIC_SOURCE_AD7606

static const seismic_source_driver_t s_driver = {
    .name =
        "AD7606",

    .transport_name =
        "SPI3",

    .sample_rate_hz =
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,

    .channel_count =
        AD7606_CHANNEL_COUNT,

    .start =
        ad7606_acquisition_start,

    .stop =
        ad7606_acquisition_stop,
};

#else
#error "No seismic acquisition source selected"
#endif


static bool s_running =
    false;


static esp_err_t seismic_acquisition_publish_status(
    bool running
)
{
    return
        device_status_set_acquisition(
            running,
            running
                ? s_driver.sample_rate_hz
                : 0,
            0,
            0
        );
}


static void seismic_acquisition_reset_failed_start(void)
{
    /* Backends own partial cleanup; stop() is intentionally idempotent. */
    s_driver.stop();

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running =
        false;

    (void)seismic_acquisition_publish_status(
        false
    );
}


esp_err_t seismic_acquisition_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Source: %s",
        s_driver.name
    );

    esp_err_t err =
        s_driver.start();

    if (err != ESP_OK) {
        seismic_acquisition_reset_failed_start();

        return err;
    }

    err =
        seismic_acquisition_publish_status(
            true
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to publish running acquisition status: %s",
            esp_err_to_name(err)
        );

        seismic_acquisition_reset_failed_start();

        return err;
    }

    s_running =
        true;

    return ESP_OK;
}


void seismic_acquisition_stop(void)
{
    if (s_running) {
        s_driver.stop();
    }

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running =
        false;

    esp_err_t err =
        seismic_acquisition_publish_status(
            false
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to publish stopped acquisition status: %s",
            esp_err_to_name(err)
        );
    }
}


bool seismic_acquisition_is_running(void)
{
    return
        s_running;
}


const char *seismic_acquisition_source_name(void)
{
    return
        s_driver.name;
}


static void seismic_acquisition_snapshot_base(
    seismic_acquisition_snapshot_t *snapshot
)
{
    memset(
        snapshot,
        0,
        sizeof(*snapshot)
    );

    snapshot->running =
        s_running;

    snapshot->source_name =
        s_driver.name;

    snapshot->transport_name =
        s_driver.transport_name;

    snapshot->sample_rate_hz =
        s_driver.sample_rate_hz;

    snapshot->channel_count =
        s_driver.channel_count;

    snapshot->core_id =
        APP_TASK_CORE_REALTIME;
}


#if !CONFIG_SEISMIC_SOURCE_AD7606
static void seismic_acquisition_snapshot_buffer(
    seismic_acquisition_snapshot_t *snapshot
)
{
    seismic_frame_t frame;
    size_t frame_count =
        0;

    seismic_buffer_info_t info;

    esp_err_t err =
        seismic_buffer_copy_latest(
            &frame,
            1,
            &frame_count,
            &info
        );

    if (err != ESP_OK) {
        return;
    }

    snapshot->samples =
        info.total_frames;

    if (frame_count == 0) {
        return;
    }

    snapshot->latest_valid =
        true;

    snapshot->last_sample_timestamp_us =
        frame.timestamp_us;

    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        snapshot->latest_uv[channel] =
            frame.channel_uv[channel];
    }
}
#endif


esp_err_t seismic_acquisition_get_snapshot(
    seismic_acquisition_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_acquisition_snapshot_base(
        snapshot
    );

#if CONFIG_SEISMIC_SOURCE_AD7606

    ad7606_acquisition_status_t status;

    esp_err_t err =
        ad7606_acquisition_get_status(
            &status
        );

    if (err != ESP_OK) {
        return err;
    }

    snapshot->running =
        status.running &&
        s_running;

    snapshot->latest_valid =
        status.latest_valid;

    snapshot->samples =
        status.samples;

    snapshot->read_errors =
        status.read_errors;

    snapshot->consecutive_failures =
        status.consecutive_failures;

    snapshot->last_sample_timestamp_us =
        status.last_sample_timestamp_us;

    snapshot->bus_clock_hz =
        AD7606_BUS_CLOCK_HZ;

    snapshot->bus_mode =
        AD7606_SPI_MODE;

    snapshot->input_range_v =
        AD7606_INPUT_RANGE_V;

    snapshot->oversampling_ratio =
        AD7606_OVERSAMPLING_RATIO;

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        snapshot->latest_uv[channel] =
            status.latest_uv[channel];
    }

#else

    seismic_acquisition_snapshot_buffer(
        snapshot
    );

#endif

    return ESP_OK;
}
