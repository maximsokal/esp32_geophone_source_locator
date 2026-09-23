#include "localization_traveltime_inversion.h"

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
 * Scores one candidate position by how well it predicts the OBSERVED
 * differential arrival times (each used pick's time minus the reference
 * pick's time) against the differential travel time that candidate implies
 * at the caller's propagation speed. Emission time never appears: it
 * cancels out of every difference, so this needs no separate unknown the
 * way the iterative TDOA solver's 4-parameter (x, y, z, t0) fit does.
 *
 * Returns the RAW weighted chi-square, not exp(-0.5*chi-square): with a
 * realistic (tight) timing_uncertainty_us, most of a multi-meter search
 * volume is many sigma away from any reasonable position, so a raw
 * likelihood underflows to exactly 0.0 for nearly the whole grid -- ranking
 * candidates by that would just pick whichever one happened to be
 * evaluated first. Chi-square never underflows, so it stays comparable
 * across the entire grid; likelihood is derived from it only once, after
 * the true minimum is known (see localization_traveltime_search).
 */
static void evaluate_candidate(
    localization_point3d_t candidate,
    const localization_observation_t *used,
    const double *weights,
    size_t used_count,
    size_t ref_index,
    double propagation_speed_mps,
    double timing_uncertainty_us,
    double *out_residual_rms_us,
    double *out_weighted_chi_square
)
{
    const double ref_dx = candidate.x_m - used[ref_index].sensor_position.x_m;
    const double ref_dy = candidate.y_m - used[ref_index].sensor_position.y_m;
    const double ref_dz = candidate.z_m - used[ref_index].sensor_position.z_m;
    const double ref_distance = sqrt(ref_dx * ref_dx + ref_dy * ref_dy + ref_dz * ref_dz);

    double sum_sq_unweighted = 0.0;
    double sum_sq_weighted = 0.0;
    size_t diff_count = 0u;

    for (size_t i = 0u; i < used_count; ++i) {
        if (i == ref_index) {
            continue;
        }

        const double dx = candidate.x_m - used[i].sensor_position.x_m;
        const double dy = candidate.y_m - used[i].sensor_position.y_m;
        const double dz = candidate.z_m - used[i].sensor_position.z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);

        const double predicted_diff_us =
            (distance - ref_distance) / propagation_speed_mps * 1.0e6;
        const double observed_diff_us =
            (double)used[i].arrival_time_us -
            (double)used[ref_index].arrival_time_us;
        const double residual_us = observed_diff_us - predicted_diff_us;

        sum_sq_unweighted += residual_us * residual_us;

        const double weighted_term = weights[i] * residual_us / timing_uncertainty_us;
        sum_sq_weighted += weighted_term * weighted_term;
        ++diff_count;
    }

    *out_residual_rms_us = sqrt(sum_sq_unweighted / (double)diff_count);
    *out_weighted_chi_square = sum_sq_weighted;
}


static bool grid_counts(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    size_t *out_x,
    size_t *out_y,
    size_t *out_z,
    size_t *out_total
)
{
    if (
        !axis_count(min_corner.x_m, max_corner.x_m, step_m, out_x) ||
        !axis_count(min_corner.y_m, max_corner.y_m, step_m, out_y) ||
        !axis_count(min_corner.z_m, max_corner.z_m, step_m, out_z) ||
        *out_x > SIZE_MAX / *out_y ||
        *out_x * *out_y > SIZE_MAX / *out_z
    ) {
        return false;
    }
    *out_total = *out_x * *out_y * *out_z;
    return true;
}


/*
 * Smallest integer multiple of `requested_step_m` whose grid fits in
 * `budget` candidates (MAX-208: a 5x5 m 3D array at 0.1 m was ~1.07 M
 * candidates, ~49 MiB with a retained surface).
 */
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
        size_t count_x, count_y, count_z, total;
        if (!grid_counts(min_corner, max_corner, step_m, &count_x, &count_y, &count_z, &total)) {
            continue;
        }
        if (total <= budget) {
            *out_step_m = step_m;
            return true;
        }
    }
    return false;
}


/* Streaming coarse pass; fills surface/chi_square only when non-NULL. */
static esp_err_t coarse_search(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    const localization_observation_t *used,
    const double *weights,
    size_t used_count,
    size_t ref_index,
    double propagation_speed_mps,
    double timing_uncertainty_us,
    const localization_candidate_filter_t *filter,
    localization_traveltime_candidate_t *surface,
    double *chi_square,
    localization_point3d_t *out_best_position,
    double *out_best_residual_rms_us,
    double *out_best_chi_square
)
{
    size_t count_x = 0u;
    size_t count_y = 0u;
    size_t count_z = 0u;
    size_t total = 0u;
    if (!grid_counts(min_corner, max_corner, step_m, &count_x, &count_y, &count_z, &total)) {
        return ESP_ERR_INVALID_ARG;
    }

    double best_chi_square = -1.0;
    localization_point3d_t best_position = { 0.0, 0.0, 0.0 };
    double best_residual = 0.0;
    size_t idx = 0u;

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
                double residual_rms_us = 0.0;
                double candidate_chi_square = 0.0;
                if (!localization_candidate_admitted(filter, &candidate)) {
                    if (surface != NULL) {
                        surface[idx].position = candidate;
                        surface[idx].residual_rms_us = INFINITY;
                        chi_square[idx] = INFINITY;
                    }
                    ++idx;
                    continue;
                }
                evaluate_candidate(
                    candidate, used, weights, used_count, ref_index,
                    propagation_speed_mps, timing_uncertainty_us,
                    &residual_rms_us, &candidate_chi_square
                );
                if (surface != NULL) {
                    surface[idx].position = candidate;
                    surface[idx].residual_rms_us = residual_rms_us;
                    chi_square[idx] = candidate_chi_square;
                }

                if (best_chi_square < 0.0 || candidate_chi_square < best_chi_square) {
                    best_chi_square = candidate_chi_square;
                    best_position = candidate;
                    best_residual = residual_rms_us;
                }
                ++idx;
            }
        }
    }

    *out_best_position = best_position;
    *out_best_residual_rms_us = best_residual;
    *out_best_chi_square = best_chi_square;
    return ESP_OK;
}


static esp_err_t refine_search(
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double step_m,
    const localization_observation_t *used,
    const double *weights,
    size_t used_count,
    size_t ref_index,
    double propagation_speed_mps,
    double timing_uncertainty_us,
    const localization_candidate_filter_t *filter,
    localization_point3d_t *inout_best_position,
    double *inout_best_residual_rms_us,
    double *inout_best_chi_square
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
                double residual_rms_us = 0.0;
                double chi_square = 0.0;
                if (!localization_candidate_admitted(filter, &candidate)) {
                    continue;
                }
                evaluate_candidate(
                    candidate, used, weights, used_count, ref_index,
                    propagation_speed_mps, timing_uncertainty_us,
                    &residual_rms_us, &chi_square
                );

                if (chi_square < *inout_best_chi_square) {
                    *inout_best_chi_square = chi_square;
                    *inout_best_position = candidate;
                    *inout_best_residual_rms_us = residual_rms_us;
                }
            }
        }
    }

    return ESP_OK;
}


esp_err_t localization_traveltime_search(
    const localization_traveltime_pick_t *picks,
    size_t pick_count,
    const localization_traveltime_search_bounds_t *coarse_bounds,
    const localization_traveltime_config_t *config,
    localization_traveltime_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (localization_traveltime_result_t) { 0 };

    if (picks == NULL || coarse_bounds == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pick_count == 0u || pick_count > LOCALIZATION_TRAVELTIME_MAX_PICKS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        !isfinite(config->propagation_speed_mps) ||
        config->propagation_speed_mps <= 0.0 ||
        !isfinite(config->timing_uncertainty_us) ||
        config->timing_uncertainty_us <= 0.0 ||
        !isfinite(coarse_bounds->step_m) ||
        coarse_bounds->step_m <= 0.0 ||
        config->refine_divisor < 2u
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

    localization_observation_t used[LOCALIZATION_TRAVELTIME_MAX_PICKS];
    double weights[LOCALIZATION_TRAVELTIME_MAX_PICKS];
    size_t used_count = 0u;

    for (size_t i = 0u; i < pick_count; ++i) {
        if (!isfinite(picks[i].weight)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (picks[i].weight > 0.0) {
            if (
                !isfinite(picks[i].observation.sensor_position.x_m) ||
                !isfinite(picks[i].observation.sensor_position.y_m) ||
                !isfinite(picks[i].observation.sensor_position.z_m)
            ) {
                return ESP_ERR_INVALID_ARG;
            }
            used[used_count] = picks[i].observation;
            weights[used_count] = picks[i].weight;
            ++used_count;
        }
    }

    result->picks_used_count = used_count;
    result->dimensionality = localization_dimensionality_classify(used_count);

    const localization_method_availability_t availability = localization_method_availability(
        result->dimensionality, LOCALIZATION_METHOD_SUPPORTS_2D_AND_3D
    );
    if (availability != LOCALIZATION_METHOD_RUNNABLE) {
        result->status = LOCALIZATION_TRAVELTIME_INSUFFICIENT_PICKS;
        return ESP_OK;
    }

    size_t ref_index = 0u;
    for (size_t i = 1u; i < used_count; ++i) {
        if (weights[i] > weights[ref_index]) {
            ref_index = i;
        }
    }

    size_t budget =
        config->max_coarse_candidates == 0u
        ? LOCALIZATION_TRAVELTIME_DEFAULT_MAX_COARSE_CANDIDATES
        : config->max_coarse_candidates;
    if (config->retain_surface && budget > LOCALIZATION_TRAVELTIME_MAX_SURFACE_CANDIDATES) {
        budget = LOCALIZATION_TRAVELTIME_MAX_SURFACE_CANDIDATES;
    }

    double coarse_step_m = coarse_bounds->step_m;
    if (!budgeted_step(
            coarse_bounds->min_corner, coarse_bounds->max_corner,
            coarse_bounds->step_m, budget, &coarse_step_m)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t count_x, count_y, count_z, surface_count;
    (void)grid_counts(
        coarse_bounds->min_corner, coarse_bounds->max_corner, coarse_step_m,
        &count_x, &count_y, &count_z, &surface_count
    );

    localization_traveltime_candidate_t *surface = NULL;
    double *chi_square = NULL;
    if (config->retain_surface) {
        surface = calloc(surface_count, sizeof(*surface));
        chi_square = calloc(surface_count, sizeof(*chi_square));
        if (surface == NULL || chi_square == NULL) {
            free(surface);
            free(chi_square);
            return ESP_ERR_NO_MEM;
        }
    }

    localization_point3d_t best_position = { 0.0, 0.0, 0.0 };
    double best_residual_rms_us = 0.0;
    double best_chi_square = -1.0;

    const esp_err_t coarse_err = coarse_search(
        coarse_bounds->min_corner, coarse_bounds->max_corner, coarse_step_m,
        used, weights, used_count, ref_index,
        config->propagation_speed_mps, config->timing_uncertainty_us,
        &config->candidate_filter,
        surface, chi_square,
        &best_position, &best_residual_rms_us, &best_chi_square
    );
    if (coarse_err != ESP_OK) {
        free(surface);
        free(chi_square);
        return coarse_err;
    }
    if (best_chi_square < 0.0) {
        /* MAX-147: the filter admitted no grid point at all. */
        free(surface);
        free(chi_square);
        result->status = LOCALIZATION_TRAVELTIME_NO_ALLOWED_CANDIDATE;
        result->coarse_step_m_used = coarse_step_m;
        return ESP_OK;
    }

    const double fine_step_m = coarse_step_m / (double)config->refine_divisor;
    const bool is_2d = coarse_bounds->min_corner.z_m == coarse_bounds->max_corner.z_m;
    const localization_point3d_t coarse_best = best_position;
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

    const esp_err_t refine_err = refine_search(
        fine_min, fine_max, fine_step_m, used, weights, used_count, ref_index,
        config->propagation_speed_mps, config->timing_uncertainty_us,
        &config->candidate_filter,
        &best_position, &best_residual_rms_us, &best_chi_square
    );
    if (refine_err != ESP_OK) {
        free(surface);
        free(chi_square);
        return refine_err;
    }

    /*
     * Only now, with the true global minimum chi-square known (coarse AND
     * fine passes), convert the retained coarse surface to a relative
     * likelihood: exp(-0.5*(chi_square_i - best_chi_square)). This stays
     * well-defined and non-degenerate everywhere, unlike an absolute
     * exp(-0.5*chi_square_i), which underflows to indistinguishable zeros
     * across almost the whole grid at realistic timing precision.
     */
    for (size_t i = 0u; surface != NULL && i < surface_count; ++i) {
        surface[i].relative_likelihood = exp(-0.5 * (chi_square[i] - best_chi_square));
    }
    free(chi_square);

    result->status = LOCALIZATION_TRAVELTIME_SOLVED;
    result->best_position = best_position;
    result->best_residual_rms_us = best_residual_rms_us;
    result->coarse_step_m_used = coarse_step_m;
    result->surface = surface;
    result->surface_count = surface != NULL ? surface_count : 0u;
    return ESP_OK;
}
