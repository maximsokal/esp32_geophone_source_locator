#include "seismic_acquisition.h"

#include <stdint.h>

#include "sdkconfig.h"

#include "ad7606_acquisition.h"
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
    uint32_t sample_rate_hz;
    esp_err_t (*start)(void);
    void (*stop)(void);

} seismic_source_driver_t;


#if CONFIG_SEISMIC_SOURCE_TEST

static const seismic_source_driver_t s_driver = {
    .name =
        "TEST",

    .sample_rate_hz =
        SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ,

    .start =
        seismic_test_generator_start,

    .stop =
        seismic_test_generator_stop,
};

#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH

static const seismic_source_driver_t s_driver = {
    .name =
        "ESP_ADC_BENCH",

    .sample_rate_hz =
        CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ,

    .start =
        geophone_adc_test_start,

    .stop =
        geophone_adc_test_stop,
};

#elif CONFIG_SEISMIC_SOURCE_AD7606

static const seismic_source_driver_t s_driver = {
    .name =
        "AD7606",

    .sample_rate_hz =
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,

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
    /*
     * Backends are expected to clean up their own partial start,
     * but stop() is intentionally idempotent and gives each source
     * one final chance to release hardware/task state.
     */
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
