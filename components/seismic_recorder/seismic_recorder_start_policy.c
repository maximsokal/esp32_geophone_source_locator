#include "seismic_recorder_start_policy.h"


esp_err_t seismic_recorder_start_validate_acquisition(
    const seismic_acquisition_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!snapshot->running || snapshot->sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}
