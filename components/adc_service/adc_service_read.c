#include "adc_service.h"

#include <stdint.h>

#include "adc_service_internal.h"


static esp_err_t adc_service_read_average_raw_with_timeout(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value,
    TickType_t mutex_timeout
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

    if (xSemaphoreTake(
        g_adc_service_mutex,
        mutex_timeout
    ) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* ADC1 has one conversion owner. Always discard one conversion while
     * holding the service mutex so a channel switch cannot leak the previous
     * channel into the returned sample. */
    int discard =
        0;

    esp_err_t err =
        adc_oneshot_read(
            g_adc_service_handle,
            channel,
            &discard
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


esp_err_t adc_service_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    return adc_service_read_average_raw_with_timeout(
        channel,
        sample_count,
        raw_value,
        portMAX_DELAY
    );
}


esp_err_t adc_service_try_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    return adc_service_read_average_raw_with_timeout(
        channel,
        sample_count,
        raw_value,
        0
    );
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
