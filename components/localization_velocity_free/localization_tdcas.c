#include "localization_tdcas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_vf_math.h"


#define TDCAS_RANK_TOLERANCE 1.0e-10

/* C(8, 5) = 56 planar or C(8, 6) = 28 depth-resolving combinations. */
#define TDCAS_MAX_COMBINATIONS 64U

/* A planar-frame squared depth may come out slightly negative from pick
 * noise for a source at the surface; beyond this fraction of the mean
 * squared distance the combination is inconsistent, not just noisy. */
#define TDCAS_NEGATIVE_DEPTH_TOLERANCE 0.05

#define TDCAS_PI 3.14159265358979323846


typedef struct
{
    localization_point3d_t position;
    double velocity_mps;

} tdcas_candidate_t;


typedef struct
{
    tdcas_candidate_t candidates[TDCAS_MAX_COMBINATIONS];
    double samples[TDCAS_MAX_COMBINATIONS];

} tdcas_workspace_t;


static bool next_combination(size_t *indices, size_t k, size_t n)
{
    for (size_t i = k; i-- > 0U;) {
        if (indices[i] < n - k + i) {
            ++indices[i];
            for (size_t j = i + 1U; j < k; ++j) {
                indices[j] = indices[j - 1U] + 1U;
            }
            return true;
        }
    }
    return false;
}


static size_t earliest_of(const double *times, const size_t *indices, size_t k)
{
    size_t reference = indices[0];
    for (size_t i = 1U; i < k; ++i) {
        if (times[indices[i]] < times[reference]) {
            reference = indices[i];
        }
    }
    return reference;
}


/* 6 sensors, unknowns (x, y, z, A = v^2, B = v^2 t0) in centered
 * coordinates. */
static bool solve_combination_3d(
    const localization_point3d_t *positions,
    const double *times,
    const size_t *indices,
    localization_point3d_t *position,
    double *velocity_mps
)
{
    const size_t r = earliest_of(times, indices, LOCALIZATION_TDCAS_MIN_SENSORS_3D);
    const localization_point3d_t sr = positions[r];
    const double tr = times[r];
    const double sr_sq = sr.x_m * sr.x_m + sr.y_m * sr.y_m + sr.z_m * sr.z_m;

    double a[5 * 5];
    double b[5];
    size_t row = 0U;
    double earliest = tr;
    for (size_t k = 0U; k < LOCALIZATION_TDCAS_MIN_SENSORS_3D; ++k) {
        const size_t i = indices[k];
        if (i == r) {
            continue;
        }
        const localization_point3d_t si = positions[i];
        a[row * 5U + 0U] = 2.0 * (si.x_m - sr.x_m);
        a[row * 5U + 1U] = 2.0 * (si.y_m - sr.y_m);
        a[row * 5U + 2U] = 2.0 * (si.z_m - sr.z_m);
        a[row * 5U + 3U] = times[i] * times[i] - tr * tr;
        a[row * 5U + 4U] = -2.0 * (times[i] - tr);
        b[row] = (si.x_m * si.x_m + si.y_m * si.y_m + si.z_m * si.z_m) - sr_sq;
        ++row;
    }

    double solution[5];
    if (!vf_least_squares(a, b, 5U, 5U, TDCAS_RANK_TOLERANCE, solution)) {
        return false;
    }
    const double v_sq = solution[3];
    if (!(v_sq > 0.0)) {
        return false;
    }
    /* d_r = v (t_r - t0) >= 0: the origin cannot follow the first arrival. */
    const double t0 = solution[4] / v_sq;
    if (!(t0 <= earliest + 1.0e-9)) {
        return false;
    }

    *position = (localization_point3d_t) { solution[0], solution[1], solution[2] };
    *velocity_mps = sqrt(v_sq);
    return true;
}


/* 5 sensors in the array's plane frame, unknowns (u, v, A, B); the depth
 * below the plane follows from the recovered distances. */
static bool solve_combination_planar(
    const double *u,
    const double *v,
    const double *times,
    const size_t *indices,
    const vf_plane_frame_t *frame,
    localization_point3d_t *position,
    double *velocity_mps
)
{
    const size_t k_count = LOCALIZATION_TDCAS_MIN_SENSORS_PLANAR;
    const size_t r = earliest_of(times, indices, k_count);
    const double tr = times[r];
    const double r_sq = u[r] * u[r] + v[r] * v[r];

    double a[4 * 4];
    double b[4];
    size_t row = 0U;
    for (size_t k = 0U; k < k_count; ++k) {
        const size_t i = indices[k];
        if (i == r) {
            continue;
        }
        a[row * 4U + 0U] = 2.0 * (u[i] - u[r]);
        a[row * 4U + 1U] = 2.0 * (v[i] - v[r]);
        a[row * 4U + 2U] = times[i] * times[i] - tr * tr;
        a[row * 4U + 3U] = -2.0 * (times[i] - tr);
        b[row] = (u[i] * u[i] + v[i] * v[i]) - r_sq;
        ++row;
    }

    double solution[4];
    if (!vf_least_squares(a, b, 4U, 4U, TDCAS_RANK_TOLERANCE, solution)) {
        return false;
    }
    const double v_sq = solution[2];
    if (!(v_sq > 0.0)) {
        return false;
    }
    const double t0 = solution[3] / v_sq;
    if (!(t0 <= tr + 1.0e-9)) {
        return false;
    }

    double depth_sq = 0.0;
    double distance_sq = 0.0;
    for (size_t k = 0U; k < k_count; ++k) {
        const size_t i = indices[k];
        const double d_sq = v_sq * (times[i] - t0) * (times[i] - t0);
        const double du = solution[0] - u[i];
        const double dv = solution[1] - v[i];
        depth_sq += d_sq - (du * du + dv * dv);
        distance_sq += d_sq;
    }
    depth_sq /= (double)k_count;
    distance_sq /= (double)k_count;
    if (depth_sq < -TDCAS_NEGATIVE_DEPTH_TOLERANCE * distance_sq) {
        return false;
    }

    /* The frame normal points up: a buried source is on the negative side. */
    const double w = -sqrt(depth_sq > 0.0 ? depth_sq : 0.0);
    *position = vf_from_plane(frame, solution[0], solution[1], w);
    *velocity_mps = sqrt(v_sq);
    return true;
}


static bool combination_admitted(
    const localization_tdcas_config_t *config,
    const localization_observation_t *observations,
    const size_t *indices,
    size_t k,
    const localization_point3d_t *candidate
)
{
    if (config == NULL) {
        return true;
    }
    if (!localization_candidate_admitted(&config->candidate_filter, candidate)) {
        return false;
    }
    for (size_t i = 0U; i < k; ++i) {
        if (!localization_ray_admitted(&config->ray_filter, &observations[indices[i]].sensor_position, candidate)) {
            return false;
        }
    }
    return true;
}


esp_err_t localization_tdcas_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_tdcas_config_t *config,
    localization_tdcas_result_t *result
)
{
    if (
        observations == NULL ||
        result == NULL ||
        observation_count == 0U ||
        observation_count > LOCALIZATION_TDCAS_MAX_SENSORS
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_point3d_t positions[LOCALIZATION_TDCAS_MAX_SENSORS];
    for (size_t i = 0U; i < observation_count; ++i) {
        positions[i] = observations[i].sensor_position;
        if (!isfinite(positions[i].x_m) || !isfinite(positions[i].y_m) || !isfinite(positions[i].z_m)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memset(result, 0, sizeof(*result));
    result->sensors_used = observation_count;

    const bool resolves_depth = localization_positions_resolve_depth(positions, observation_count);
    result->planar_layout = !resolves_depth;
    const size_t k = resolves_depth ? LOCALIZATION_TDCAS_MIN_SENSORS_3D : LOCALIZATION_TDCAS_MIN_SENSORS_PLANAR;
    if (observation_count < k) {
        result->status = LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS;
        return ESP_OK;
    }

    /* Conditioning: times in seconds after the first arrival, positions
     * relative to the array centroid (the equations are invariant under
     * both shifts). */
    int64_t first_us = observations[0].arrival_time_us;
    localization_point3d_t centroid = { 0.0, 0.0, 0.0 };
    for (size_t i = 0U; i < observation_count; ++i) {
        if (observations[i].arrival_time_us < first_us) {
            first_us = observations[i].arrival_time_us;
        }
        centroid.x_m += positions[i].x_m / (double)observation_count;
        centroid.y_m += positions[i].y_m / (double)observation_count;
        centroid.z_m += positions[i].z_m / (double)observation_count;
    }
    double times[LOCALIZATION_TDCAS_MAX_SENSORS];
    localization_point3d_t centered[LOCALIZATION_TDCAS_MAX_SENSORS];
    for (size_t i = 0U; i < observation_count; ++i) {
        times[i] = (double)(observations[i].arrival_time_us - first_us) * 1.0e-6;
        centered[i] = (localization_point3d_t) {
            positions[i].x_m - centroid.x_m,
            positions[i].y_m - centroid.y_m,
            positions[i].z_m - centroid.z_m,
        };
    }

    vf_plane_frame_t frame;
    double u[LOCALIZATION_TDCAS_MAX_SENSORS];
    double v[LOCALIZATION_TDCAS_MAX_SENSORS];
    if (!resolves_depth) {
        if (!vf_fit_plane(positions, observation_count, &frame)) {
            result->status = LOCALIZATION_TDCAS_NO_VALID_COMBINATION;
            return ESP_OK;
        }
        for (size_t i = 0U; i < observation_count; ++i) {
            double w = 0.0;
            vf_to_plane(&frame, positions[i], &u[i], &v[i], &w);
        }
    }

    /* Heap, not stack: this runs on the HTTPS task (see localization_vf_math.h). */
    tdcas_workspace_t *workspace = malloc(sizeof(*workspace));
    if (workspace == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tdcas_candidate_t *candidates = workspace->candidates;
    size_t candidate_count = 0U;
    size_t indices[LOCALIZATION_TDCAS_MIN_SENSORS_3D];
    for (size_t i = 0U; i < k; ++i) {
        indices[i] = i;
    }

    do {
        ++result->combinations_total;

        localization_point3d_t position;
        double velocity = 0.0;
        bool solved = false;
        if (resolves_depth) {
            localization_point3d_t subset[LOCALIZATION_TDCAS_MIN_SENSORS_3D];
            for (size_t i = 0U; i < k; ++i) {
                subset[i] = positions[indices[i]];
            }
            /* A near-planar 6-subset of a 3D array cannot resolve z. */
            solved =
                localization_positions_resolve_depth(subset, k) &&
                solve_combination_3d(centered, times, indices, &position, &velocity);
            position.x_m += centroid.x_m;
            position.y_m += centroid.y_m;
            position.z_m += centroid.z_m;
        } else {
            solved = solve_combination_planar(u, v, times, indices, &frame, &position, &velocity);
        }
        if (!solved) {
            continue;
        }

        ++result->combinations_valid;
        if (!combination_admitted(config, observations, indices, k, &position)) {
            ++result->combinations_filtered;
            continue;
        }
        if (candidate_count < TDCAS_MAX_COMBINATIONS) {
            candidates[candidate_count].position = position;
            candidates[candidate_count].velocity_mps = velocity;
            ++candidate_count;
        }
    } while (next_combination(indices, k, observation_count));

    if (candidate_count == 0U) {
        result->status = result->combinations_valid > 0U
            ? LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION
            : LOCALIZATION_TDCAS_NO_VALID_COMBINATION;
        free(workspace);
        return ESP_OK;
    }

    /* Comprehensive solution: logistic location of each coordinate. */
    double *samples = workspace->samples;
    double location[4];
    double scale[4];
    for (size_t axis = 0U; axis < 4U; ++axis) {
        for (size_t i = 0U; i < candidate_count; ++i) {
            const localization_point3d_t p = candidates[i].position;
            samples[i] =
                axis == 0U ? p.x_m :
                axis == 1U ? p.y_m :
                axis == 2U ? p.z_m :
                candidates[i].velocity_mps;
        }
        if (!vf_fit_logistic(samples, candidate_count, &location[axis], &scale[axis])) {
            result->status = LOCALIZATION_TDCAS_NO_VALID_COMBINATION;
            free(workspace);
            return ESP_OK;
        }
    }
    free(workspace);

    result->position = (localization_point3d_t) { location[0], location[1], location[2] };
    result->position_scale_m = (localization_point3d_t) { scale[0], scale[1], scale[2] };
    /* Logistic variance = (s pi)^2 / 3 per axis. */
    result->spread_m = sqrt((scale[0] * scale[0] + scale[1] * scale[1] + scale[2] * scale[2]) * TDCAS_PI * TDCAS_PI / 3.0);
    result->velocity_scale_mps = scale[3];

    /* Velocity and origin time against every sensor at the final point. */
    double distances[LOCALIZATION_TDCAS_MAX_SENSORS];
    for (size_t i = 0U; i < observation_count; ++i) {
        distances[i] = vf_distance(result->position, positions[i]);
    }
    vf_time_fit_t fit;
    double velocity = location[3];
    double t0 = 0.0;
    double rms_s = 0.0;
    if (
        vf_fit_travel_times(times, distances, NULL, observation_count, 1.0, 0.0, -1.0, &fit) &&
        fit.slowness_s_per_m > 0.0
    ) {
        velocity = 1.0 / fit.slowness_s_per_m;
        t0 = fit.t0_s;
        rms_s = fit.rms_s;
    } else {
        const double slowness = 1.0 / location[3];
        (void)vf_fit_travel_times(times, distances, NULL, observation_count, 1.0, slowness, 0.0, &fit);
        t0 = fit.t0_s;
        rms_s = fit.rms_s;
    }

    result->velocity_mps = velocity;
    result->emission_time_us = first_us + (int64_t)llround(t0 * 1.0e6);
    result->rms_residual_us = rms_s * 1.0e6;
    result->status = LOCALIZATION_TDCAS_SOLVED;
    return ESP_OK;
}
