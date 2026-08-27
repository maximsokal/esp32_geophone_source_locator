#include "geophone_adc_test_internal.h"

#include "esp_adc/adc_cali.h"


#define GEOPHONE_CAL_SPAN_RAW    64
#define GEOPHONE_ADC_MAX_RAW     4095


esp_err_t geophone_adc_calibrate_scale(
    float baseline_raw,
    float *uv_per_count
)
{
    if (uv_per_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int center =
        (int)(
            baseline_raw +
            0.5f
        );

    int low_raw =
        center -
        GEOPHONE_CAL_SPAN_RAW;

    int high_raw =
        center +
        GEOPHONE_CAL_SPAN_RAW;

    if (low_raw < 0) {
        low_raw = 0;
    }

    if (high_raw > GEOPHONE_ADC_MAX_RAW) {
        high_raw =
            GEOPHONE_ADC_MAX_RAW;
    }

    if (high_raw <= low_raw) {
        return ESP_ERR_INVALID_STATE;
    }

    int low_mv =
        0;

    int high_mv =
        0;

    ESP_ERROR_CHECK(
        adc_cali_raw_to_voltage(
            g_geophone_adc_cali,
            low_raw,
            &low_mv
        )
    );

    ESP_ERROR_CHECK(
        adc_cali_raw_to_voltage(
            g_geophone_adc_cali,
            high_raw,
            &high_mv
        )
    );

    *uv_per_count =
        (
            (float)(
                high_mv -
                low_mv
            ) *
            1000.0f
        ) /
        (float)(
            high_raw -
            low_raw
        );

    return ESP_OK;
}
