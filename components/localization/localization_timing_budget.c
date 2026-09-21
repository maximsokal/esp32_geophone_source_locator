#include "localization_timing_budget.h"

#include <math.h>
#include <stdlib.h>


#define LOCALIZATION_TIMING_BUDGET_MAX_OBSERVATIONS 32u


esp_err_t localization_timing_jitter_error_bound(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    int64_t jitter_us,
    double *position_error_m_out
)
{
    if (
        observations == NULL ||
        config == NULL ||
        position_error_m_out == NULL ||
        observation_count == 0u ||
        observation_count > LOCALIZATION_TIMING_BUDGET_MAX_OBSERVATIONS
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_result_t baseline;
    esp_err_t err = localization_tdoa_solve(
        observations, observation_count, config, initial_guess, &baseline
    );
    if (err != ESP_OK) {
        return err;
    }

    localization_observation_t perturbed[LOCALIZATION_TIMING_BUDGET_MAX_OBSERVATIONS];
    for (size_t i = 0u; i < observation_count; ++i) {
        perturbed[i] = observations[i];
        perturbed[i].arrival_time_us +=
            (i % 2u == 0u) ? jitter_us : -jitter_us;
    }

    localization_result_t jittered;
    err = localization_tdoa_solve(
        perturbed,
        observation_count,
        config,
        &baseline.source_position,
        &jittered
    );
    if (err != ESP_OK) {
        return err;
    }

    const double dx = jittered.source_position.x_m - baseline.source_position.x_m;
    const double dy = jittered.source_position.y_m - baseline.source_position.y_m;
    const double dz = jittered.source_position.z_m - baseline.source_position.z_m;

    *position_error_m_out = sqrt(dx * dx + dy * dy + dz * dz);
    return ESP_OK;
}
