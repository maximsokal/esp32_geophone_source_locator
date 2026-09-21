#include "localization_semblance.h"

#include <math.h>
#include <stdlib.h>


static size_t axis_count(
    double min_value,
    double max_value,
    double step_m
)
{
    if (max_value <= min_value) {
        return 1u;
    }
    return (size_t)llround((max_value - min_value) / step_m) + 1u;
}


/*
 * Neidell & Taner semblance: for the analysis window aligned to `candidate`,
 * S = (sum over time of (sum over channels)^2) / (channel_count * sum over
 * channels and time of value^2). 1.0 = perfectly coherent stack, 0.0 = no
 * coherence (or no signal at all).
 *
 * A candidate whose predicted shift pushes a channel's window partly or
 * fully outside that channel's recorded buffer is not rejected outright --
 * out-of-buffer samples are treated as silence, which naturally penalizes
 * (rather than crashes on) a candidate with no real evidence there.
 */
static bool compute_semblance_at(
    localization_point3d_t candidate,
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_config_t *config,
    double *out_semblance
)
{
    const size_t window_len = 2u * config->window_half_span_samples + 1u;
    double *stack = calloc(window_len, sizeof(double));
    if (stack == NULL) {
        return false;
    }

    double sum_squares_all = 0.0;

    for (size_t ch = 0u; ch < channel_count; ++ch) {
        const double dx = candidate.x_m - channels[ch].sensor_position.x_m;
        const double dy = candidate.y_m - channels[ch].sensor_position.y_m;
        const double dz = candidate.z_m - channels[ch].sensor_position.z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        const double predicted_delay_s =
            distance / config->propagation_speed_mps;
        const long shift_samples = (long)llround(
            predicted_delay_s * (double)channels[ch].sample_rate_hz
        );

        const long window_start =
            (long)config->reference_sample_index +
            shift_samples -
            (long)config->window_half_span_samples;

        for (size_t t = 0u; t < window_len; ++t) {
            const long index = window_start + (long)t;
            float value = 0.0f;
            if (index >= 0 && (size_t)index < channels[ch].sample_count) {
                value = channels[ch].samples[index];
            }
            stack[t] += (double)value;
            sum_squares_all += (double)value * (double)value;
        }
    }

    double sum_stack_squares = 0.0;
    for (size_t t = 0u; t < window_len; ++t) {
        sum_stack_squares += stack[t] * stack[t];
    }
    free(stack);

    *out_semblance =
        sum_squares_all > 0.0
        ? sum_stack_squares / ((double)channel_count * sum_squares_all)
        : 0.0;
    return true;
}


static esp_err_t search_grid(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_config_t *config,
    localization_semblance_result_t *result
)
{
    const size_t count_x = axis_count(min_corner.x_m, max_corner.x_m, step_m);
    const size_t count_y = axis_count(min_corner.y_m, max_corner.y_m, step_m);
    const size_t count_z = axis_count(min_corner.z_m, max_corner.z_m, step_m);

    for (size_t ix = 0u; ix < count_x; ++ix) {
        const double x =
            count_x == 1u ? min_corner.x_m : min_corner.x_m + (double)ix * step_m;
        for (size_t iy = 0u; iy < count_y; ++iy) {
            const double y =
                count_y == 1u ? min_corner.y_m : min_corner.y_m + (double)iy * step_m;
            for (size_t iz = 0u; iz < count_z; ++iz) {
                const double z =
                    count_z == 1u
                    ? min_corner.z_m
                    : min_corner.z_m + (double)iz * step_m;

                const localization_point3d_t candidate = { x, y, z };
                double semblance = 0.0;
                if (
                    !compute_semblance_at(
                        candidate, channels, channel_count, config, &semblance
                    )
                ) {
                    return ESP_ERR_NO_MEM;
                }

                result->candidates_searched += 1u;
                if (semblance > result->best_semblance) {
                    result->best_semblance = semblance;
                    result->best_position = candidate;
                    result->found = true;
                }
                if (semblance < result->worst_semblance) {
                    result->worst_semblance = semblance;
                }
            }
        }
    }

    return ESP_OK;
}


esp_err_t localization_semblance_search(
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_search_bounds_t *coarse_bounds,
    const localization_semblance_config_t *config,
    localization_semblance_result_t *result
)
{
    if (
        channels == NULL ||
        coarse_bounds == NULL ||
        config == NULL ||
        result == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (channel_count < 2u || channel_count > LOCALIZATION_SEMBLANCE_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        config->propagation_speed_mps <= 0.0 ||
        coarse_bounds->step_m <= 0.0 ||
        config->window_half_span_samples == 0u ||
        config->refine_divisor < 2u
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 1u; i < channel_count; ++i) {
        if (
            channels[i].sample_rate_hz != channels[0].sample_rate_hz ||
            channels[i].sample_count != channels[0].sample_count
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    *result = (localization_semblance_result_t) {
        .found = false,
        .best_position = { 0.0, 0.0, 0.0 },
        .best_semblance = -1.0,
        .worst_semblance = 2.0,
        .candidates_searched = 0u
    };

    esp_err_t err = search_grid(
        coarse_bounds->min_corner,
        coarse_bounds->max_corner,
        coarse_bounds->step_m,
        channels,
        channel_count,
        config,
        result
    );
    if (err != ESP_OK || !result->found) {
        return err;
    }

    const double fine_step_m = coarse_bounds->step_m / (double)config->refine_divisor;
    const bool is_2d = coarse_bounds->min_corner.z_m == coarse_bounds->max_corner.z_m;

    const localization_point3d_t coarse_best = result->best_position;
    const localization_point3d_t fine_min = {
        .x_m = coarse_best.x_m - coarse_bounds->step_m,
        .y_m = coarse_best.y_m - coarse_bounds->step_m,
        .z_m = is_2d ? coarse_best.z_m : coarse_best.z_m - coarse_bounds->step_m
    };
    const localization_point3d_t fine_max = {
        .x_m = coarse_best.x_m + coarse_bounds->step_m,
        .y_m = coarse_best.y_m + coarse_bounds->step_m,
        .z_m = is_2d ? coarse_best.z_m : coarse_best.z_m + coarse_bounds->step_m
    };

    return search_grid(
        fine_min, fine_max, fine_step_m, channels, channel_count, config, result
    );
}
