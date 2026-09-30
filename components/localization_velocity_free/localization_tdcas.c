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
    uint32_t members; /* bit i: observation i is in this combination */
    bool admitted;    /* passed the candidate filter */

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


/* Logistic ("comprehensive") fusion of the admitted combinations that use
 * none of the excluded sensors. False when no combination qualifies. */
static bool fuse_combinations(
    tdcas_workspace_t *workspace,
    size_t candidate_count,
    uint32_t excluded,
    double location[4],
    double scale[4],
    size_t *used_out
)
{
    size_t used = 0U;
    for (size_t axis = 0U; axis < 4U; ++axis) {
        used = 0U;
        for (size_t i = 0U; i < candidate_count; ++i) {
            const tdcas_candidate_t *c = &workspace->candidates[i];
            if (!c->admitted || (c->members & excluded) != 0U) {
                continue;
            }
            workspace->samples[used++] =
                axis == 0U ? c->position.x_m :
                axis == 1U ? c->position.y_m :
                axis == 2U ? c->position.z_m :
                c->velocity_mps;
        }
        if (used == 0U || !vf_fit_logistic(workspace->samples, used, &location[axis], &scale[axis])) {
            return false;
        }
    }
    *used_out = used;
    return true;
}


static bool velocity_in_bounds(double velocity, const localization_velocity_bounds_t *bounds)
{
    if (bounds == NULL || !bounds->set) {
        return true;
    }
    if (bounds->v_min_mps == bounds->v_max_mps) {
        /* Microsecond pick quantization perturbs fixed-speed subset fits;
         * the final travel-time refit still fixes the requested speed. */
        return fabs(velocity - bounds->v_min_mps) <= 0.01 * bounds->v_min_mps;
    }
    return velocity >= bounds->v_min_mps && velocity <= bounds->v_max_mps;
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

    const localization_velocity_bounds_t *velocity_bounds =
        config != NULL ? config->velocity_bounds : NULL;
    const bool bounds_set = velocity_bounds != NULL && velocity_bounds->set;
    if (bounds_set && localization_velocity_bounds_validate(velocity_bounds) != LOCALIZATION_VELOCITY_BOUNDS_VALID) {
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
        result->velocity_status = bounds_set
            ? LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE
            : LOCALIZATION_VELOCITY_STATUS_UNKNOWN;
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

        localization_point3d_t position = { 0.0, 0.0, 0.0 };
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
            if (solved) {
                position.x_m += centroid.x_m;
                position.y_m += centroid.y_m;
                position.z_m += centroid.z_m;
            }
        } else {
            solved = solve_combination_planar(u, v, times, indices, &frame, &position, &velocity);
        }
        if (!solved) {
            continue;
        }

        ++result->combinations_valid;
        if (bounds_set && !velocity_in_bounds(velocity, velocity_bounds)) {
            ++result->combinations_out_of_range;
            continue;
        }
        if (candidate_count < TDCAS_MAX_COMBINATIONS) {
            uint32_t members = 0U;
            for (size_t i = 0U; i < k; ++i) {
                members |= 1U << indices[i];
            }
            candidates[candidate_count].position = position;
            candidates[candidate_count].velocity_mps = velocity;
            candidates[candidate_count].members = members;
            candidates[candidate_count].admitted =
                config == NULL || localization_candidate_admitted(&config->candidate_filter, &position);
            ++candidate_count;
        }
    } while (next_combination(indices, k, observation_count));

    /*
     * Comprehensive solution: logistic location of each coordinate over the
     * admitted combinations. Rays are then checked per SENSOR against that
     * consensus position (never against a combination's own solution, which
     * a delayed pick can bias until its own ray looks clear): a sensor whose
     * straight ray crosses empty space is excluded with every combination
     * containing it, and the fusion repeats until no new sensor is excluded.
     */
    double location[4];
    double scale[4];
    size_t used = 0U;
    uint32_t excluded = 0U;
    bool fused = fuse_combinations(workspace, candidate_count, excluded, location, scale, &used);
    for (size_t pass = 0U; fused && config != NULL && pass < observation_count; ++pass) {
        const localization_point3d_t consensus = { location[0], location[1], location[2] };
        uint32_t newly = 0U;
        for (size_t i = 0U; i < observation_count; ++i) {
            if (
                (excluded & (1U << i)) == 0U &&
                !localization_ray_admitted(&config->ray_filter, &positions[i], &consensus)
            ) {
                newly |= 1U << i;
            }
        }
        if (newly == 0U) {
            break;
        }
        excluded |= newly;
        fused = fuse_combinations(workspace, candidate_count, excluded, location, scale, &used);
    }
    free(workspace);

    result->sensor_mask_obstructed = excluded;
    for (uint32_t bits = excluded; bits != 0U; bits &= bits - 1U) {
        --result->sensors_used;
    }
    const size_t in_range = result->combinations_valid - result->combinations_out_of_range;
    result->combinations_filtered = in_range - (fused ? used : 0U);
    if (!fused) {
        result->status = result->combinations_valid > 0U
            ? LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION
            : LOCALIZATION_TDCAS_NO_VALID_COMBINATION;
        if (bounds_set) {
            result->velocity_status = result->combinations_valid == 0U || in_range * 2U < result->combinations_valid
                ? LOCALIZATION_VELOCITY_STATUS_INCONSISTENT
                : LOCALIZATION_VELOCITY_STATUS_UNKNOWN;
        }
        return ESP_OK;
    }

    result->position = (localization_point3d_t) { location[0], location[1], location[2] };
    result->position_scale_m = (localization_point3d_t) { scale[0], scale[1], scale[2] };
    /* Logistic variance = (s pi)^2 / 3 per axis. */
    result->spread_m = sqrt((scale[0] * scale[0] + scale[1] * scale[1] + scale[2] * scale[2]) * TDCAS_PI * TDCAS_PI / 3.0);
    result->velocity_scale_mps = scale[3];

    /* MAX-311: velocity, origin time and RMS are refitted at the final point
     * over the ray-ADMITTED sensors only. An excluded sensor was excluded
     * because its straight-ray travel time t0 + |x - s| / v cannot hold, so
     * keeping it would bias v and t0 and inflate the RMS -- which drives the
     * bounded-mode INCONSISTENT status (MAX-301). Fusion needs at least k
     * admitted sensors, so the admitted set is never smaller than k here. */
    double admitted_times[LOCALIZATION_TDCAS_MAX_SENSORS];
    double distances[LOCALIZATION_TDCAS_MAX_SENSORS];
    size_t admitted_count = 0U;
    for (size_t i = 0U; i < observation_count; ++i) {
        if ((excluded & (1U << i)) != 0U) continue;
        admitted_times[admitted_count] = times[i];
        distances[admitted_count] = vf_distance(result->position, positions[i]);
        ++admitted_count;
    }
    vf_time_fit_t fit = { 0 };
    bool refit_at_bound = false;
    if (admitted_count < 2U || !vf_fit_travel_times_bounded(
        admitted_times, distances, NULL, admitted_count, 1.0, location[3],
        velocity_bounds, &fit, &refit_at_bound
    )) {
        result->status = LOCALIZATION_TDCAS_NO_VALID_COMBINATION;
        result->velocity_status = bounds_set
            ? LOCALIZATION_VELOCITY_STATUS_INCONSISTENT
            : LOCALIZATION_VELOCITY_STATUS_UNKNOWN;
        return ESP_OK;
    }
    const double velocity = 1.0 / fit.slowness_s_per_m;

    result->velocity_mps = velocity;
    result->emission_time_us = first_us + (int64_t)llround(fit.t0_s * 1.0e6);
    result->rms_residual_us = fit.rms_s * 1.0e6;
    result->status = LOCALIZATION_TDCAS_SOLVED;
    if (bounds_set) {
        const bool inconsistent =
            in_range * 2U < result->combinations_valid ||
            (config->timing_uncertainty_us > 0.0 &&
                result->rms_residual_us > 3.0 * config->timing_uncertainty_us);
        result->velocity_status = inconsistent
            ? LOCALIZATION_VELOCITY_STATUS_INCONSISTENT
            : velocity_bounds->v_min_mps < velocity_bounds->v_max_mps && refit_at_bound
                ? LOCALIZATION_VELOCITY_STATUS_AT_BOUND
                : LOCALIZATION_VELOCITY_STATUS_OK;
    }
    return ESP_OK;
}
