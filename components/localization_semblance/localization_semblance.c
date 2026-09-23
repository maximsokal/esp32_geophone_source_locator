#include "localization_semblance.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>


static bool axis_count(
    double min_value,
    double max_value,
    double step_m,
    size_t *out_count
)
{
    if (
        out_count == NULL ||
        !isfinite(min_value) ||
        !isfinite(max_value) ||
        !isfinite(step_m) ||
        step_m <= 0.0 ||
        max_value < min_value
    ) {
        return false;
    }

    if (max_value == min_value) {
        *out_count = 1u;
        return true;
    }

    const double ratio = (max_value - min_value) / step_m;
    if (!isfinite(ratio) || ratio < 0.0 || ratio >= (double)(LLONG_MAX - 1LL)) {
        return false;
    }

    const long long increments = llround(ratio);
    if (increments < 0 || increments >= LLONG_MAX - 1LL) {
        return false;
    }

    *out_count = (size_t)increments + 1u;
    return *out_count != 0u;
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
static bool grid_total(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    size_t *out_total
)
{
    size_t count_x = 0u;
    size_t count_y = 0u;
    size_t count_z = 0u;
    if (
        !axis_count(min_corner.x_m, max_corner.x_m, step_m, &count_x) ||
        !axis_count(min_corner.y_m, max_corner.y_m, step_m, &count_y) ||
        !axis_count(min_corner.z_m, max_corner.z_m, step_m, &count_z) ||
        count_x > SIZE_MAX / count_y ||
        count_x * count_y > SIZE_MAX / count_z
    ) {
        return false;
    }
    *out_total = count_x * count_y * count_z;
    return true;
}


/* Smallest integer multiple of the requested step whose grid fits the
 * candidate budget (MAX-208). */
static bool budgeted_step(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double requested_step_m,
    size_t budget,
    double *out_step_m
)
{
    for (unsigned factor = 1u; factor <= 100000u; ++factor) {
        const double step_m = requested_step_m * (double)factor;
        size_t total = 0u;
        if (grid_total(min_corner, max_corner, step_m, &total) && total <= budget) {
            *out_step_m = step_m;
            return true;
        }
    }
    return false;
}


static void compute_semblance_at(
    localization_point3d_t candidate,
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_config_t *config,
    double *stack,
    double *out_semblance
)
{
    const size_t window_len = 2u * config->window_half_span_samples + 1u;
    for (size_t t = 0u; t < window_len; ++t) {
        stack[t] = 0.0;
    }

    /* MAX-207: reference_sample_index is the EARLIEST observed arrival, which
     * already contains the (unknown) origin time plus the travel time to the
     * nearest sensor. Only differential delays are therefore added: the
     * channel with the shortest predicted travel time is aligned exactly on
     * the reference, every other channel later by its extra travel time.
     * Adding the full travel time again would shift every window late by
     * this candidate's own minimum travel time. */
    long shift_samples[LOCALIZATION_SEMBLANCE_MAX_CHANNELS];
    long min_shift = LONG_MAX;
    for (size_t ch = 0u; ch < channel_count; ++ch) {
        const double dx = candidate.x_m - channels[ch].sensor_position.x_m;
        const double dy = candidate.y_m - channels[ch].sensor_position.y_m;
        const double dz = candidate.z_m - channels[ch].sensor_position.z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        const double predicted_delay_s =
            distance / config->propagation_speed_mps;
        shift_samples[ch] = (long)llround(
            predicted_delay_s * (double)channels[ch].sample_rate_hz
        );
        if (shift_samples[ch] < min_shift) {
            min_shift = shift_samples[ch];
        }
    }

    double sum_squares_all = 0.0;

    for (size_t ch = 0u; ch < channel_count; ++ch) {
        const long window_start =
            (long)config->reference_sample_index +
            (shift_samples[ch] - min_shift) -
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

    *out_semblance =
        sum_squares_all > 0.0
        ? sum_stack_squares / ((double)channel_count * sum_squares_all)
        : 0.0;
}


static esp_err_t search_grid(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_config_t *config,
    double *stack,
    localization_semblance_result_t *result
)
{
    size_t count_x = 0u;
    size_t count_y = 0u;
    size_t count_z = 0u;
    if (
        !axis_count(min_corner.x_m, max_corner.x_m, step_m, &count_x) ||
        !axis_count(min_corner.y_m, max_corner.y_m, step_m, &count_y) ||
        !axis_count(min_corner.z_m, max_corner.z_m, step_m, &count_z) ||
        count_x > SIZE_MAX / count_y ||
        count_x * count_y > SIZE_MAX / count_z
    ) {
        return ESP_ERR_INVALID_ARG;
    }

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
                if (!localization_candidate_admitted(&config->candidate_filter, &candidate)) {
                    continue;
                }
                double semblance = 0.0;
                compute_semblance_at(
                    candidate, channels, channel_count, config, stack, &semblance
                );

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
        !isfinite(config->propagation_speed_mps) ||
        config->propagation_speed_mps <= 0.0 ||
        !isfinite(coarse_bounds->step_m) ||
        coarse_bounds->step_m <= 0.0 ||
        config->window_half_span_samples == 0u ||
        config->refine_divisor < 2u ||
        config->window_half_span_samples > (SIZE_MAX - 1u) / 2u
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        !isfinite(coarse_bounds->min_corner.x_m) ||
        !isfinite(coarse_bounds->min_corner.y_m) ||
        !isfinite(coarse_bounds->min_corner.z_m) ||
        !isfinite(coarse_bounds->max_corner.x_m) ||
        !isfinite(coarse_bounds->max_corner.y_m) ||
        !isfinite(coarse_bounds->max_corner.z_m) ||
        coarse_bounds->max_corner.x_m < coarse_bounds->min_corner.x_m ||
        coarse_bounds->max_corner.y_m < coarse_bounds->min_corner.y_m ||
        coarse_bounds->max_corner.z_m < coarse_bounds->min_corner.z_m
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

    for (size_t i = 0u; i < channel_count; ++i) {
        if (
            channels[i].samples == NULL ||
            channels[i].sample_count == 0u ||
            channels[i].sample_rate_hz == 0u ||
            !isfinite(channels[i].sensor_position.x_m) ||
            !isfinite(channels[i].sensor_position.y_m) ||
            !isfinite(channels[i].sensor_position.z_m)
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t sample = 0u; sample < channels[i].sample_count; ++sample) {
            if (!isfinite(channels[i].samples[sample])) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    *result = (localization_semblance_result_t) {
        .found = false,
        .best_position = { 0.0, 0.0, 0.0 },
        .best_semblance = -1.0,
        .worst_semblance = 2.0,
        .candidates_searched = 0u
    };

    const size_t budget =
        config->max_coarse_candidates == 0u
        ? LOCALIZATION_SEMBLANCE_DEFAULT_MAX_COARSE_CANDIDATES
        : config->max_coarse_candidates;
    double coarse_step_m = coarse_bounds->step_m;
    if (!budgeted_step(
            coarse_bounds->min_corner, coarse_bounds->max_corner,
            coarse_bounds->step_m, budget, &coarse_step_m)) {
        return ESP_ERR_INVALID_ARG;
    }
    result->coarse_step_m_used = coarse_step_m;

    /* One stack buffer for the whole search instead of one allocation per
     * candidate position. */
    double *stack = calloc(2u * config->window_half_span_samples + 1u, sizeof(double));
    if (stack == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = search_grid(
        coarse_bounds->min_corner,
        coarse_bounds->max_corner,
        coarse_step_m,
        channels,
        channel_count,
        config,
        stack,
        result
    );
    if (err != ESP_OK || !result->found) {
        free(stack);
        return err;
    }

    const double fine_step_m = coarse_step_m / (double)config->refine_divisor;
    const bool is_2d = coarse_bounds->min_corner.z_m == coarse_bounds->max_corner.z_m;

    const localization_point3d_t coarse_best = result->best_position;
    const localization_point3d_t fine_min = {
        .x_m = coarse_best.x_m - coarse_step_m,
        .y_m = coarse_best.y_m - coarse_step_m,
        .z_m = is_2d ? coarse_best.z_m : coarse_best.z_m - coarse_step_m
    };
    const localization_point3d_t fine_max = {
        .x_m = coarse_best.x_m + coarse_step_m,
        .y_m = coarse_best.y_m + coarse_step_m,
        .z_m = is_2d ? coarse_best.z_m : coarse_best.z_m + coarse_step_m
    };

    err = search_grid(
        fine_min, fine_max, fine_step_m, channels, channel_count, config, stack, result
    );
    free(stack);
    return err;
}
