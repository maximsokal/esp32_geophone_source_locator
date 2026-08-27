#include "adc_service.h"

#include <stdint.h>

#include "adc_service_internal.h"


static bool s_last_channel_valid =
    false;

static adc_channel_t s_last_channel;


static esp_err_t adc_service_prepare_channel(
    adc_channel_t channel
)
{
    if (
        s_last_channel_valid &&
        s_last_channel ==
        channel
    ) {
        return ESP_OK;
    }

    int discard =
        0;

    esp_err_t err =
        adc_oneshot_read(
            g_adc_service_handle,
            channel,
            &discard
        );

    if (err == ESP_OK) {
        s_last_channel =
            channel;

        s_last_channel_valid =
            true;
    }

    return err;
}


esp_err_t adc_service_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    if (!g_adc_service_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        raw_value == NULL ||
        sample_count == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(
        g_adc_service_mutex,
        portMAX_DELAY
    );

    esp_err_t err =
        adc_service_prepare_channel(
            channel
        );

    int64_t sum =
        0;

    for (
        size_t i = 0;
        err == ESP_OK &&
        i < sample_count;
        ++i
    ) {
        int raw =
            0;

        err =
            adc_oneshot_read(
                g_adc_service_handle,
                channel,
                &raw
            );

        sum +=
            raw;
    }

    xSemaphoreGive(
        g_adc_service_mutex
    );

    if (err != ESP_OK) {
        return err;
    }

    *raw_value =
        (int)(
            sum /
            (int64_t)sample_count
        );

    return ESP_OK;
}


esp_err_t adc_service_read_raw(
    adc_channel_t channel,
    int *raw_value
)
{
    return
        adc_service_read_average_raw(
            channel,
            1,
            raw_value
        );
}
