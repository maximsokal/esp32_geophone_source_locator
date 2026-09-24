#include "localization_psafuvs.h"

#include <math.h>
#include <string.h>

#include "localization_vf_math.h"


#define PSAFUVS_RANK_TOLERANCE 1.0e-10
#define PSAFUVS_DEFAULT_ITERATIONS 30U
#define PSAFUVS_DEFAULT_CONVERGENCE_M 1.0e-4
#define PSAFUVS_NEGATIVE_DEPTH_TOLERANCE 0.05
#define PSAFUVS_PARAMETERS 6U


typedef struct
{
    size_t count;
    size_t index[LOCALIZATION_PSAFUVS_MAX_SENSORS];
    localization_point3d_t position[LOCALIZATION_PSAFUVS_MAX_SENSORS];
    double tp[LOCALIZATION_PSAFUVS_MAX_SENSORS]; /* s after the first P arrival */
    double ts[LOCALIZATION_PSAFUVS_MAX_SENSORS];

} psafuvs_subset_t;


typedef struct
{
    localization_psafuvs_status_t status;
    bool planar;
    vf_plane_frame_t frame;
    double theta[PSAFUVS_PARAMETERS]; /* x, y, z, t0, 1/vP, 1/vS */
    double k_mps;
    double rms_s;
    uint8_t iterations;
    bool converged;

} psafuvs_fit_t;


localization_psafuvs_config_t localization_psafuvs_default_config(void)
{
    return (localization_psafuvs_config_t) {
        .max_iterations = PSAFUVS_DEFAULT_ITERATIONS,
        .convergence_m = PSAFUVS_DEFAULT_CONVERGENCE_M,
    };
}


static double cost_of(const psafuvs_subset_t *subset, const double *theta, double *jacobian, double *residuals)
{
    const localization_point3d_t source = { theta[0], theta[1], theta[2] };
    double cost = 0.0;
    for (size_t i = 0U; i < subset->count; ++i) {
        const double d = vf_distance(source, subset->position[i]);
        const double rp = theta[3] + theta[4] * d - subset->tp[i];
        const double rs = theta[3] + theta[5] * d - subset->ts[i];
        cost += rp * rp + rs * rs;
        if (residuals != NULL) {
            residuals[2U * i] = rp;
            residuals[2U * i + 1U] = rs;
        }
        if (jacobian != NULL) {
            const double g[3] = {
                d > 0.0 ? (source.x_m - subset->position[i].x_m) / d : 0.0,
                d > 0.0 ? (source.y_m - subset->position[i].y_m) / d : 0.0,
                d > 0.0 ? (source.z_m - subset->position[i].z_m) / d : 0.0,
            };
            double *jp = &jacobian[(2U * i) * PSAFUVS_PARAMETERS];
            double *js = &jacobian[(2U * i + 1U) * PSAFUVS_PARAMETERS];
            for (size_t a = 0U; a < 3U; ++a) {
                jp[a] = theta[4] * g[a];
                js[a] = theta[5] * g[a];
            }
            jp[3] = 1.0; jp[4] = d;   jp[5] = 0.0;
            js[3] = 1.0; js[4] = 0.0; js[5] = d;
        }
    }
    return cost;
}


/* A planar layout cannot tell a source below the surface from its mirror
 * image above it; keep the physical (buried) one. */
static void keep_below_plane(const psafuvs_fit_t *fit, double *theta)
{
    if (!fit->planar) {
        return;
    }
    const localization_point3d_t p = { theta[0], theta[1], theta[2] };
    double u = 0.0, v = 0.0, w = 0.0;
    vf_to_plane(&fit->frame, p, &u, &v, &w);
    if (w > 0.0) {
        const localization_point3d_t mirrored = vf_from_plane(&fit->frame, u, v, -w);
        theta[0] = mirrored.x_m;
        theta[1] = mirrored.y_m;
        theta[2] = mirrored.z_m;
    }
}


static bool analytical_position(const psafuvs_subset_t *subset, psafuvs_fit_t *fit, localization_point3d_t *position)
{
    const size_t n = subset->count;
    double a[LOCALIZATION_PSAFUVS_MAX_SENSORS * 5U];
    double b[LOCALIZATION_PSAFUVS_MAX_SENSORS];
    double solution[5];

    if (!fit->planar) {
        localization_point3d_t centroid = { 0.0, 0.0, 0.0 };
        for (size_t i = 0U; i < n; ++i) {
            centroid.x_m += subset->position[i].x_m / (double)n;
            centroid.y_m += subset->position[i].y_m / (double)n;
            centroid.z_m += subset->position[i].z_m / (double)n;
        }
        for (size_t i = 0U; i < n; ++i) {
            const double x = subset->position[i].x_m - centroid.x_m;
            const double y = subset->position[i].y_m - centroid.y_m;
            const double z = subset->position[i].z_m - centroid.z_m;
            const double dt = subset->ts[i] - subset->tp[i];
            a[i * 5U + 0U] = 2.0 * x;
            a[i * 5U + 1U] = 2.0 * y;
            a[i * 5U + 2U] = 2.0 * z;
            a[i * 5U + 3U] = dt * dt;
            a[i * 5U + 4U] = -1.0;
            b[i] = x * x + y * y + z * z;
        }
        if (!vf_least_squares(a, b, n, 5U, PSAFUVS_RANK_TOLERANCE, solution) || !(solution[3] > 0.0)) {
            return false;
        }
        fit->k_mps = sqrt(solution[3]);
        *position = (localization_point3d_t) {
            solution[0] + centroid.x_m, solution[1] + centroid.y_m, solution[2] + centroid.z_m,
        };
        return true;
    }

    double u[LOCALIZATION_PSAFUVS_MAX_SENSORS];
    double v[LOCALIZATION_PSAFUVS_MAX_SENSORS];
    for (size_t i = 0U; i < n; ++i) {
        double w = 0.0;
        vf_to_plane(&fit->frame, subset->position[i], &u[i], &v[i], &w);
        const double dt = subset->ts[i] - subset->tp[i];
        a[i * 4U + 0U] = 2.0 * u[i];
        a[i * 4U + 1U] = 2.0 * v[i];
        a[i * 4U + 2U] = dt * dt;
        a[i * 4U + 3U] = -1.0;
        b[i] = u[i] * u[i] + v[i] * v[i];
    }
    if (!vf_least_squares(a, b, n, 4U, PSAFUVS_RANK_TOLERANCE, solution) || !(solution[2] > 0.0)) {
        return false;
    }
    const double k_sq = solution[2];
    double depth_sq = 0.0;
    double distance_sq = 0.0;
    for (size_t i = 0U; i < n; ++i) {
        const double dt = subset->ts[i] - subset->tp[i];
        const double du = solution[0] - u[i];
        const double dv = solution[1] - v[i];
        depth_sq += k_sq * dt * dt - (du * du + dv * dv);
        distance_sq += k_sq * dt * dt;
    }
    depth_sq /= (double)n;
    distance_sq /= (double)n;
    if (depth_sq < -PSAFUVS_NEGATIVE_DEPTH_TOLERANCE * distance_sq) {
        return false;
    }
    fit->k_mps = sqrt(k_sq);
    *position = vf_from_plane(&fit->frame, solution[0], solution[1], -sqrt(depth_sq > 0.0 ? depth_sq : 0.0));
    return true;
}


static void solve_subset(const psafuvs_subset_t *subset, const localization_psafuvs_config_t *config, psafuvs_fit_t *fit)
{
    memset(fit, 0, sizeof(*fit));
    const size_t n = subset->count;

    fit->planar = !localization_positions_resolve_depth(subset->position, n);
    const size_t needed = fit->planar ? LOCALIZATION_PSAFUVS_MIN_SENSORS_PLANAR : LOCALIZATION_PSAFUVS_MIN_SENSORS_3D;
    if (n < needed) {
        fit->status = LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS;
        return;
    }
    if (fit->planar && !vf_fit_plane(subset->position, n, &fit->frame)) {
        fit->status = LOCALIZATION_PSAFUVS_DEGENERATE;
        return;
    }

    localization_point3d_t position;
    if (!analytical_position(subset, fit, &position)) {
        fit->status = LOCALIZATION_PSAFUVS_DEGENERATE;
        return;
    }

    /* (t0, 1/vP, 1/vS) given the analytical distances: linear. */
    double a[2U * LOCALIZATION_PSAFUVS_MAX_SENSORS * 3U];
    double b[2U * LOCALIZATION_PSAFUVS_MAX_SENSORS];
    for (size_t i = 0U; i < n; ++i) {
        const double d = vf_distance(position, subset->position[i]);
        a[(2U * i) * 3U + 0U] = 1.0; a[(2U * i) * 3U + 1U] = d;   a[(2U * i) * 3U + 2U] = 0.0;
        a[(2U * i + 1U) * 3U + 0U] = 1.0; a[(2U * i + 1U) * 3U + 1U] = 0.0; a[(2U * i + 1U) * 3U + 2U] = d;
        b[2U * i] = subset->tp[i];
        b[2U * i + 1U] = subset->ts[i];
    }
    double timing[3];
    if (!vf_least_squares(a, b, 2U * n, 3U, PSAFUVS_RANK_TOLERANCE, timing)) {
        fit->status = LOCALIZATION_PSAFUVS_DEGENERATE;
        return;
    }
    double theta[PSAFUVS_PARAMETERS] = {
        position.x_m, position.y_m, position.z_m, timing[0], timing[1], timing[2],
    };

    /* Levenberg-Marquardt over all 2N arrivals. */
    double jacobian[2U * LOCALIZATION_PSAFUVS_MAX_SENSORS * PSAFUVS_PARAMETERS];
    double residuals[2U * LOCALIZATION_PSAFUVS_MAX_SENSORS];
    double cost = cost_of(subset, theta, NULL, NULL);
    double lambda = 1.0e-3;
    const uint8_t max_iterations = config != NULL ? config->max_iterations : PSAFUVS_DEFAULT_ITERATIONS;
    const double convergence = config != NULL && config->convergence_m > 0.0
        ? config->convergence_m
        : PSAFUVS_DEFAULT_CONVERGENCE_M;
    fit->converged = max_iterations == 0U;

    for (uint8_t iteration = 0U; iteration < max_iterations; ++iteration) {
        (void)cost_of(subset, theta, jacobian, residuals);
        double jtj[PSAFUVS_PARAMETERS * PSAFUVS_PARAMETERS] = { 0.0 };
        double jtr[PSAFUVS_PARAMETERS] = { 0.0 };
        for (size_t row = 0U; row < 2U * n; ++row) {
            const double *j = &jacobian[row * PSAFUVS_PARAMETERS];
            for (size_t p = 0U; p < PSAFUVS_PARAMETERS; ++p) {
                jtr[p] -= j[p] * residuals[row];
                for (size_t q = 0U; q < PSAFUVS_PARAMETERS; ++q) {
                    jtj[p * PSAFUVS_PARAMETERS + q] += j[p] * j[q];
                }
            }
        }

        bool accepted = false;
        double step_m = 0.0;
        for (int attempt = 0; attempt < 12 && !accepted; ++attempt) {
            double damped[PSAFUVS_PARAMETERS * PSAFUVS_PARAMETERS];
            memcpy(damped, jtj, sizeof(damped));
            for (size_t p = 0U; p < PSAFUVS_PARAMETERS; ++p) {
                const double diagonal = jtj[p * PSAFUVS_PARAMETERS + p];
                damped[p * PSAFUVS_PARAMETERS + p] = diagonal * (1.0 + lambda) + 1.0e-30;
            }
            double delta[PSAFUVS_PARAMETERS];
            if (vf_solve_square(damped, jtr, PSAFUVS_PARAMETERS, delta)) {
                double trial[PSAFUVS_PARAMETERS];
                for (size_t p = 0U; p < PSAFUVS_PARAMETERS; ++p) {
                    trial[p] = theta[p] + delta[p];
                }
                keep_below_plane(fit, trial);
                const double trial_cost = cost_of(subset, trial, NULL, NULL);
                if (isfinite(trial_cost) && trial_cost < cost) {
                    step_m = sqrt(
                        (trial[0] - theta[0]) * (trial[0] - theta[0]) +
                        (trial[1] - theta[1]) * (trial[1] - theta[1]) +
                        (trial[2] - theta[2]) * (trial[2] - theta[2])
                    );
                    memcpy(theta, trial, sizeof(theta));
                    cost = trial_cost;
                    lambda = lambda * 0.1 > 1.0e-12 ? lambda * 0.1 : 1.0e-12;
                    accepted = true;
                }
            }
            if (!accepted) {
                lambda *= 10.0;
            }
        }

        fit->iterations = (uint8_t)(iteration + 1U);
        if (!accepted || step_m <= convergence) {
            /* No downhill step left: at the (local) least-squares minimum. */
            fit->converged = true;
            break;
        }
    }

    memcpy(fit->theta, theta, sizeof(theta));
    fit->rms_s = sqrt(cost / (double)(2U * n));
    if (!(theta[4] > 0.0) || !(theta[5] > theta[4])) {
        fit->status = LOCALIZATION_PSAFUVS_NONPHYSICAL_VELOCITIES;
        return;
    }
    fit->status = LOCALIZATION_PSAFUVS_SOLVED;
}


static void build_subset(
    const localization_psafuvs_observation_t *observations,
    size_t count,
    uint32_t mask,
    int64_t first_us,
    psafuvs_subset_t *subset
)
{
    subset->count = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if ((mask & (1U << i)) == 0U) {
            continue;
        }
        const size_t k = subset->count++;
        subset->index[k] = i;
        subset->position[k] = observations[i].sensor_position;
        subset->tp[k] = (double)(observations[i].p_arrival_time_us - first_us) * 1.0e-6;
        subset->ts[k] = (double)(observations[i].s_arrival_time_us - first_us) * 1.0e-6;
    }
}


esp_err_t localization_psafuvs_solve(
    const localization_psafuvs_observation_t *observations,
    size_t observation_count,
    const localization_psafuvs_config_t *config,
    localization_psafuvs_result_t *result
)
{
    if (
        observations == NULL ||
        result == NULL ||
        observation_count == 0U ||
        observation_count > LOCALIZATION_PSAFUVS_MAX_SENSORS
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t first_us = observations[0].p_arrival_time_us;
    for (size_t i = 0U; i < observation_count; ++i) {
        const localization_point3d_t p = observations[i].sensor_position;
        if (
            !isfinite(p.x_m) || !isfinite(p.y_m) || !isfinite(p.z_m) ||
            observations[i].s_arrival_time_us <= observations[i].p_arrival_time_us
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        if (observations[i].p_arrival_time_us < first_us) {
            first_us = observations[i].p_arrival_time_us;
        }
    }

    memset(result, 0, sizeof(*result));

    uint32_t mask = (1U << observation_count) - 1U;
    uint32_t obstructed = 0U;
    psafuvs_subset_t subset;
    psafuvs_fit_t fit;

    /* Solve, drop sensors whose straight ray is obstructed, re-solve; each
     * pass removes at least one sensor, so this terminates. */
    for (;;) {
        build_subset(observations, observation_count, mask, first_us, &subset);
        solve_subset(&subset, config, &fit);
        if (fit.status != LOCALIZATION_PSAFUVS_SOLVED) {
            if (obstructed != 0U && fit.status == LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS) {
                fit.status = LOCALIZATION_PSAFUVS_RAYS_OBSTRUCTED;
            }
            break;
        }
        const localization_point3d_t source = { fit.theta[0], fit.theta[1], fit.theta[2] };
        uint32_t newly_obstructed = 0U;
        for (size_t k = 0U; config != NULL && k < subset.count; ++k) {
            if (!localization_ray_admitted(&config->ray_filter, &subset.position[k], &source)) {
                newly_obstructed |= 1U << subset.index[k];
            }
        }
        if (newly_obstructed == 0U) {
            break;
        }
        obstructed |= newly_obstructed;
        mask &= ~newly_obstructed;
    }

    result->planar_layout = fit.planar;
    result->sensors_used = subset.count;
    result->sensor_mask_used = mask;
    result->sensor_mask_obstructed = obstructed;
    result->iterations = fit.iterations;
    result->converged = fit.converged;
    result->status = fit.status;
    if (fit.status != LOCALIZATION_PSAFUVS_SOLVED) {
        return ESP_OK;
    }

    result->position = (localization_point3d_t) { fit.theta[0], fit.theta[1], fit.theta[2] };
    if (config != NULL && !localization_candidate_admitted(&config->candidate_filter, &result->position)) {
        result->status = LOCALIZATION_PSAFUVS_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }
    result->emission_time_us = first_us + (int64_t)llround(fit.theta[3] * 1.0e6);
    result->p_velocity_mps = 1.0 / fit.theta[4];
    result->s_velocity_mps = 1.0 / fit.theta[5];
    /* K from the fitted velocities (the analytical K only seeded the fit). */
    result->virtual_velocity_mps =
        result->p_velocity_mps * result->s_velocity_mps / (result->p_velocity_mps - result->s_velocity_mps);
    result->rms_residual_us = fit.rms_s * 1.0e6;
    return ESP_OK;
}
