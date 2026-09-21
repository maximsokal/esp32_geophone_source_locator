#include "localization_propagation_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


esp_err_t localization_propagation_calibrate_from_taps(
    const double known_distance_m[],
    const double measured_travel_time_s[],
    size_t sample_count,
    const char *model_name,
    localization_propagation_model_t *result
)
{
    if (
        known_distance_m == NULL ||
        measured_travel_time_s == NULL ||
        result == NULL ||
        sample_count < 3u ||
        sample_count > LOCALIZATION_PROPAGATION_MAX_TAP_SAMPLES
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0u; i < sample_count; ++i) {
        if (
            !isfinite(known_distance_m[i]) ||
            known_distance_m[i] <= 0.0 ||
            !isfinite(measured_travel_time_s[i]) ||
            measured_travel_time_s[i] < 0.0
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Least-squares fit of time = slope * distance + intercept. */
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        sum_x += known_distance_m[i];
        sum_y += measured_travel_time_s[i];
    }
    const double mean_x = sum_x / (double)sample_count;
    const double mean_y = sum_y / (double)sample_count;

    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        const double dx = known_distance_m[i] - mean_x;
        const double dy = measured_travel_time_s[i] - mean_y;
        sum_xx += dx * dx;
        sum_xy += dx * dy;
    }

    if (sum_xx <= 0.0) {
        /* All distances identical: slope is undefined. */
        return ESP_ERR_INVALID_STATE;
    }

    const double slope = sum_xy / sum_xx;
    const double intercept = mean_y - slope * mean_x;

    if (!isfinite(slope) || slope <= 0.0) {
        /* Non-positive slope implies zero/negative/infinite velocity. */
        return ESP_ERR_INVALID_STATE;
    }

    double residual_sum_squares = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        const double predicted = slope * known_distance_m[i] + intercept;
        const double residual = measured_travel_time_s[i] - predicted;
        residual_sum_squares += residual * residual;
    }

    const size_t degrees_of_freedom = sample_count - 2u;
    const double residual_variance =
        residual_sum_squares / (double)degrees_of_freedom;
    const double slope_standard_error =
        sqrt(residual_variance / sum_xx);

    const double velocity_mps = 1.0 / slope;
    if (!isfinite(velocity_mps)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Error propagation for v = 1/slope: sigma_v = |dv/d(slope)| * sigma_slope
     * = sigma_slope / slope^2. */
    const double uncertainty_mps =
        slope_standard_error / (slope * slope);

    memset(result, 0, sizeof(*result));
    if (model_name != NULL) {
        snprintf(result->name, sizeof(result->name), "%s", model_name);
    }
    result->velocity_mps = velocity_mps;
    result->uncertainty_mps = isfinite(uncertainty_mps) ? uncertainty_mps : 0.0;

    return ESP_OK;
}


esp_err_t localization_propagation_velocity_error_bound(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    double velocity_error_mps,
    double *position_error_m_out
)
{
    if (
        observations == NULL ||
        config == NULL ||
        position_error_m_out == NULL ||
        observation_count == 0u ||
        !isfinite(velocity_error_mps)
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

    localization_config_t perturbed_config = *config;
    perturbed_config.propagation_speed_mps += velocity_error_mps;
    if (perturbed_config.propagation_speed_mps <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_result_t perturbed;
    err = localization_tdoa_solve(
        observations,
        observation_count,
        &perturbed_config,
        &baseline.source_position,
        &perturbed
    );
    if (err != ESP_OK) {
        return err;
    }

    const double dx = perturbed.source_position.x_m - baseline.source_position.x_m;
    const double dy = perturbed.source_position.y_m - baseline.source_position.y_m;
    const double dz = perturbed.source_position.z_m - baseline.source_position.z_m;

    *position_error_m_out = sqrt(dx * dx + dy * dy + dz * dz);
    return ESP_OK;
}
