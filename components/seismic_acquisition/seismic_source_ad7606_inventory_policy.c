#include "seismic_source_ad7606_inventory_policy.h"

#include <stdio.h>


static seismic_source_ad7606_start_failure_t failure_result(
    hw_module_state_t state,
    const char *reason
)
{
    seismic_source_ad7606_start_failure_t result = {
        .state = state,
    };
    (void)snprintf(result.reason, sizeof(result.reason), "%s", reason);
    return result;
}


static seismic_source_ad7606_start_failure_t unresolved_failure(esp_err_t err)
{
    const char *error_name = esp_err_to_name(err);
    if (error_name == NULL) {
        error_name = "unknown";
    }

    seismic_source_ad7606_start_failure_t result = {
        .state = HW_STATE_UNKNOWN,
    };
    (void)snprintf(
        result.reason,
        sizeof(result.reason),
        "start failed: %s",
        error_name
    );
    return result;
}


seismic_source_ad7606_start_failure_t seismic_source_ad7606_classify_start_failure(
    esp_err_t err,
    seismic_source_ad7606_start_failure_stage_t stage
)
{
    if (err == ESP_ERR_INVALID_STATE &&
        stage == SEISMIC_SOURCE_AD7606_START_FAILURE_RAIL_GATE) {
        return failure_result(HW_STATE_UNKNOWN, "blocked: 5V rail low");
    }

    if (err == ESP_ERR_TIMEOUT &&
        stage == SEISMIC_SOURCE_AD7606_START_FAILURE_BUSY_TIMEOUT) {
        return failure_result(HW_STATE_ABSENT, "no BUSY response");
    }

    /* PCNT resource setup can also return NOT_FOUND, unrelated to ADC presence. */
    if (err == ESP_ERR_INVALID_STATE ||
        err == ESP_ERR_TIMEOUT ||
        err == ESP_ERR_NOT_FOUND ||
        err == ESP_ERR_INVALID_ARG) {
        return unresolved_failure(err);
    }

    return failure_result(HW_STATE_FAULT, "init failed");
}
