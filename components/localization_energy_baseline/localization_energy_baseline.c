#include "localization_energy_baseline.h"


esp_err_t localization_energy_baseline_estimate(
    const localization_energy_baseline_channel_t *channels,
    size_t channel_count,
    localization_energy_baseline_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (localization_energy_baseline_result_t) { 0 };

    if (channels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel_count == 0u || channel_count > LOCALIZATION_ENERGY_BASELINE_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    double total_weighted_energy = 0.0;
    double max_weighted_energy = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_z = 0.0;
    size_t used_count = 0u;

    for (size_t i = 0u; i < channel_count; ++i) {
        if (channels[i].weight <= 0.0) {
            continue;
        }
        if (channels[i].energy < 0.0) {
            return ESP_ERR_INVALID_ARG;
        }

        const double weighted_energy = channels[i].weight * channels[i].energy;
        sum_x += weighted_energy * channels[i].sensor_position.x_m;
        sum_y += weighted_energy * channels[i].sensor_position.y_m;
        sum_z += weighted_energy * channels[i].sensor_position.z_m;
        total_weighted_energy += weighted_energy;
        if (weighted_energy > max_weighted_energy) {
            max_weighted_energy = weighted_energy;
        }
        ++used_count;
    }

    result->channels_used_count = used_count;

    if (used_count == 0u || total_weighted_energy <= 0.0) {
        return ESP_OK;
    }

    result->found = true;
    result->position = (localization_point3d_t) {
        .x_m = sum_x / total_weighted_energy,
        .y_m = sum_y / total_weighted_energy,
        .z_m = sum_z / total_weighted_energy
    };
    result->energy_concentration = max_weighted_energy / total_weighted_energy;
    return ESP_OK;
}
