#include "seismic_recorder_start_policy.h"

#include <limits.h>

#include "esp_timer.h"


static uint64_t seismic_recorder_start_max_sample_age_us(
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U) {
        return 0U;
    }

    const uint64_t sample_period_us =
        (1000000ULL + (uint64_t)sample_rate_hz - 1ULL) /
        (uint64_t)sample_rate_hz;

    if (sample_period_us > UINT64_MAX /
        (uint64_t)SEISMIC_RECORDER_START_FRESH_SAMPLE_PERIODS) {
        return UINT64_MAX;
    }

    return sample_period_us *
        (uint64_t)SEISMIC_RECORDER_START_FRESH_SAMPLE_PERIODS;
}


esp_err_t seismic_recorder_start_validate_acquisition_at(
    const seismic_acquisition_snapshot_t *snapshot,
    int64_t now_us
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!snapshot->running || snapshot->sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        !snapshot->latest_valid ||
        snapshot->samples == 0U ||
        snapshot->last_sample_timestamp_us < 0 ||
        now_us < 0 ||
        now_us < snapshot->last_sample_timestamp_us
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t sample_age_us = (uint64_t)(
        now_us - snapshot->last_sample_timestamp_us
    );
    if (
        sample_age_us >
        seismic_recorder_start_max_sample_age_us(snapshot->sample_rate_hz)
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


esp_err_t seismic_recorder_start_validate_acquisition(
    const seismic_acquisition_snapshot_t *snapshot
)
{
    return seismic_recorder_start_validate_acquisition_at(
        snapshot,
        esp_timer_get_time()
    );
}
