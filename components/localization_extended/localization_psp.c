#include "localization_psp.h"

#include <math.h>
#include <string.h>

#include "localization_ext_fit.h"


#define PSP_DEFAULT_LATTICE_POINTS 8000U
#define PSP_DEFAULT_ITERATIONS 60U


/* Parameter layout: x, y, [z], t0, [p], q. */
typedef struct
{
    const localization_psp_observation_t *observations;
    size_t count;
    const localization_extended_bounds_t *bounds;
    bool planar;
    bool p_free;
    bool p_prior;
    double p0;                  /* prior / fixed slowness, s/m */
    double p_sigma;             /* prior sigma of the slowness */
    double sigma_p_s;
    double sigma_ps_s[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double p_arrival_s[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double s_minus_p_s[LOCALIZATION_EXTENDED_MAX_SENSORS];

    size_t index_t0;
    size_t index_p;
    size_t index_q;
    size_t param_count;

} psp_problem_t;


static localization_point3d_t psp_position(const psp_problem_t *problem, const double *params)
{
    localization_point3d_t point = { params[0], params[1], problem->bounds->min_corner.z_m };
    if (!problem->planar) {
        point.z_m = params[2];
    }
    return point;
}


static double psp_slowness(const psp_problem_t *problem, const double *params)
{
    return problem->p_free ? params[problem->index_p] : problem->p0;
}


static bool psp_row(size_t row, const double *params, double *residual, double *gradient, void *context)
{
    const psp_problem_t *problem = context;
    const size_t n = problem->count;
    if (row == 2U * n) {
        if (!problem->p_prior) {
            return false;
        }
        *residual = (params[problem->index_p] - problem->p0) / problem->p_sigma;
        gradient[problem->index_p] = 1.0 / problem->p_sigma;
        return true;
    }
    const size_t i = row % n;
    const bool s_row = row >= n;
    if (s_row && !problem->observations[i].s_valid) {
        return false;
    }
    const localization_point3d_t point = psp_position(problem, params);
    const localization_point3d_t sensor = problem->observations[i].sensor_position;
    double distance = ext_distance(point, sensor);
    if (distance < 1.0e-9) {
        distance = 1.0e-9;
    }
    double factor = 0.0;
    if (s_row) {
        const double sigma = problem->sigma_ps_s[i];
        const double q = params[problem->index_q];
        *residual = (problem->s_minus_p_s[i] - q * distance) / sigma;
        gradient[problem->index_q] = -distance / sigma;
        factor = -q / (sigma * distance);
    } else {
        const double sigma = problem->sigma_p_s;
        const double p = psp_slowness(problem, params);
        *residual = (problem->p_arrival_s[i] - params[problem->index_t0] - p * distance) / sigma;
        gradient[problem->index_t0] = -1.0 / sigma;
        if (problem->p_free) {
            gradient[problem->index_p] = -distance / sigma;
        }
        factor = -p / (sigma * distance);
    }
    gradient[0] = factor * (point.x_m - sensor.x_m);
    gradient[1] = factor * (point.y_m - sensor.y_m);
    if (!problem->planar) {
        gradient[2] = factor * (point.z_m - sensor.z_m);
    }
    return true;
}


static void psp_project(double *params, void *context)
{
    const psp_problem_t *problem = context;
    const localization_point3d_t clamped = ext_clamp_to_bounds(problem->bounds, psp_position(problem, params));
    params[0] = clamped.x_m;
    params[1] = clamped.y_m;
    if (!problem->planar) {
        params[2] = clamped.z_m;
    }
    /* Slownesses stay positive: vS < vP and both finite. */
    if (problem->p_free && params[problem->index_p] < 1.0e-6) {
        params[problem->index_p] = 1.0e-6;
    }
    if (params[problem->index_q] < 1.0e-9) {
        params[problem->index_q] = 1.0e-9;
    }
}


/* Closed-form (t0, p, q) at a fixed position, and the resulting cost. */
static double linear_fit_at(const psp_problem_t *problem, localization_point3d_t point, double *t0, double *p, double *q)
{
    const size_t n = problem->count;
    double distance[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double sqd = 0.0;
    double sdd = 0.0;
    for (size_t i = 0U; i < n; ++i) {
        distance[i] = ext_distance(point, problem->observations[i].sensor_position);
        if (problem->observations[i].s_valid) {
            const double w = 1.0 / (problem->sigma_ps_s[i] * problem->sigma_ps_s[i]);
            sqd += w * problem->s_minus_p_s[i] * distance[i];
            sdd += w * distance[i] * distance[i];
        }
    }
    *q = sdd > 0.0 ? sqd / sdd : 0.0;

    if (problem->p_free) {
        /* Normal equations of t_P = t0 + p D (+ prior on p). */
        const double w = 1.0 / (problem->sigma_p_s * problem->sigma_p_s);
        double a11 = 0.0, a12 = 0.0, a22 = 0.0, b1 = 0.0, b2 = 0.0;
        for (size_t i = 0U; i < n; ++i) {
            a11 += w;
            a12 += w * distance[i];
            a22 += w * distance[i] * distance[i];
            b1 += w * problem->p_arrival_s[i];
            b2 += w * distance[i] * problem->p_arrival_s[i];
        }
        if (problem->p_prior) {
            const double wp = 1.0 / (problem->p_sigma * problem->p_sigma);
            a22 += wp;
            b2 += wp * problem->p0;
        }
        const double det = a11 * a22 - a12 * a12;
        if (!(fabs(det) > 0.0)) {
            return INFINITY;
        }
        *t0 = (b1 * a22 - a12 * b2) / det;
        *p = (a11 * b2 - a12 * b1) / det;
    } else {
        *p = problem->p0;
        double sum = 0.0;
        for (size_t i = 0U; i < n; ++i) {
            sum += problem->p_arrival_s[i] - *p * distance[i];
        }
        *t0 = sum / (double)n;
    }
    if (!(*p > 0.0) || !(*q > 0.0)) {
        return INFINITY;
    }

    double cost = 0.0;
    for (size_t i = 0U; i < n; ++i) {
        const double rp = (problem->p_arrival_s[i] - *t0 - *p * distance[i]) / problem->sigma_p_s;
        cost += rp * rp;
        if (problem->observations[i].s_valid) {
            const double rs = (problem->s_minus_p_s[i] - *q * distance[i]) / problem->sigma_ps_s[i];
            cost += rs * rs;
        }
    }
    if (problem->p_prior) {
        const double rp = (*p - problem->p0) / problem->p_sigma;
        cost += rp * rp;
    }
    return cost;
}


esp_err_t localization_psp_solve(
    const localization_psp_observation_t *observations,
    size_t observation_count,
    const localization_extended_bounds_t *bounds,
    const localization_psp_config_t *config,
    localization_psp_result_t *result
)
{
    if (
        observations == NULL || bounds == NULL || config == NULL || result == NULL ||
        observation_count == 0U || observation_count > LOCALIZATION_EXTENDED_MAX_SENSORS ||
        !localization_extended_bounds_valid(bounds) ||
        !(config->timing_uncertainty_us > 0.0) || !isfinite(config->timing_uncertainty_us) ||
        !isfinite(config->p_velocity_sigma_mps) ||
        (config->p_velocity_sigma_mps >= 0.0 &&
         (!(config->p_velocity_mps > 0.0) || !isfinite(config->p_velocity_mps)))
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < observation_count; ++i) {
        const localization_psp_observation_t *o = &observations[i];
        if (
            !isfinite(o->sensor_position.x_m) || !isfinite(o->sensor_position.y_m) ||
            !isfinite(o->sensor_position.z_m) ||
            (o->s_valid && o->s_arrival_time_us <= o->p_arrival_time_us) ||
            (o->s_valid && !isfinite(o->s_minus_p_sigma_us))
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memset(result, 0, sizeof(*result));
    result->planar = localization_extended_bounds_planar(bounds);

    psp_problem_t problem;
    memset(&problem, 0, sizeof(problem));
    problem.observations = observations;
    problem.count = observation_count;
    problem.bounds = bounds;
    problem.planar = result->planar;
    problem.p_free = config->p_velocity_sigma_mps != 0.0;
    problem.p_prior = config->p_velocity_sigma_mps > 0.0;
    if (config->p_velocity_sigma_mps >= 0.0) {
        problem.p0 = 1.0 / config->p_velocity_mps;
        /* A velocity sigma as a slowness sigma (first order). */
        problem.p_sigma = config->p_velocity_sigma_mps / (config->p_velocity_mps * config->p_velocity_mps);
    }
    problem.sigma_p_s = config->timing_uncertainty_us * 1.0e-6;

    int64_t earliest = observations[0].p_arrival_time_us;
    for (size_t i = 1U; i < observation_count; ++i) {
        if (observations[i].p_arrival_time_us < earliest) {
            earliest = observations[i].p_arrival_time_us;
        }
    }
    size_t s_used = 0U;
    for (size_t i = 0U; i < observation_count; ++i) {
        problem.p_arrival_s[i] = (double)(observations[i].p_arrival_time_us - earliest) * 1.0e-6;
        if (observations[i].s_valid) {
            problem.s_minus_p_s[i] =
                (double)(observations[i].s_arrival_time_us - observations[i].p_arrival_time_us) * 1.0e-6;
            problem.sigma_ps_s[i] = observations[i].s_minus_p_sigma_us > 0.0
                ? observations[i].s_minus_p_sigma_us * 1.0e-6
                : sqrt(2.0) * problem.sigma_p_s;
            result->s_mask_used |= 1U << i;
            ++s_used;
        }
    }

    const size_t spatial = problem.planar ? 2U : 3U;
    problem.index_t0 = spatial;
    problem.index_p = spatial + 1U;
    problem.index_q = problem.p_free ? spatial + 2U : spatial + 1U;
    problem.param_count = problem.index_q + 1U;
    const size_t rows = observation_count + s_used + (problem.p_prior ? 1U : 0U);
    result->degrees_of_freedom = (int32_t)rows - (int32_t)problem.param_count;
    result->p_used = observation_count;
    result->s_used = s_used;

    if (s_used == 0U) {
        result->status = LOCALIZATION_PSP_NO_S_PICK;
        return ESP_OK;
    }
    if (rows < problem.param_count) {
        result->status = LOCALIZATION_PSP_INSUFFICIENT_OBSERVATIONS;
        return ESP_OK;
    }

    ext_lattice_t lattice;
    if (!ext_lattice_init(
            bounds, config->max_lattice_points > 0U ? config->max_lattice_points : PSP_DEFAULT_LATTICE_POINTS,
            &lattice
        )) {
        return ESP_ERR_INVALID_ARG;
    }
    double best_cost = INFINITY;
    double params[EXT_MAX_PARAMS] = { 0 };
    bool any_admitted = false;
    const size_t total = ext_lattice_count(&lattice);
    for (size_t index = 0U; index < total; ++index) {
        const localization_point3d_t point = ext_lattice_point(&lattice, index);
        if (!localization_candidate_admitted(&config->candidate_filter, &point)) {
            continue;
        }
        any_admitted = true;
        double t0 = 0.0, p = 0.0, q = 0.0;
        const double cost = linear_fit_at(&problem, point, &t0, &p, &q);
        if (cost < best_cost) {
            best_cost = cost;
            params[0] = point.x_m;
            params[1] = point.y_m;
            if (!problem.planar) {
                params[2] = point.z_m;
            }
            params[problem.index_t0] = t0;
            if (problem.p_free) {
                params[problem.index_p] = p;
            }
            params[problem.index_q] = q;
        }
    }
    if (!isfinite(best_cost)) {
        result->status = any_admitted
            ? LOCALIZATION_PSP_NONPHYSICAL_VELOCITIES
            : LOCALIZATION_PSP_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }

    double seed[EXT_MAX_PARAMS];
    memcpy(seed, params, sizeof(seed));
    double scale[EXT_MAX_PARAMS];
    for (size_t k = 0U; k < problem.param_count; ++k) {
        scale[k] = 1.0;
    }
    scale[problem.index_t0] = 1.0e-3;
    scale[problem.index_q] = 1.0e-3;
    if (problem.p_free) {
        scale[problem.index_p] = 1.0e-3;
    }
    ext_lm_result_t lm;
    const size_t iterations = config->max_iterations > 0U ? config->max_iterations : PSP_DEFAULT_ITERATIONS;
    if (
        !ext_lm(problem.param_count, 2U * observation_count + 1U, psp_row, psp_project, &problem,
                scale, 1.0e-7, iterations, params, &lm)
    ) {
        memcpy(params, seed, sizeof(params));
        memset(&lm, 0, sizeof(lm));
    }
    localization_point3d_t position = psp_position(&problem, params);
    if (!localization_candidate_admitted(&config->candidate_filter, &position)) {
        memcpy(params, seed, sizeof(params));
        position = psp_position(&problem, params);
    }

    const double p = psp_slowness(&problem, params);
    const double q = params[problem.index_q];
    result->p_velocity_mps = 1.0 / p;
    result->s_velocity_mps = 1.0 / (p + q);
    if (!(p > 0.0) || !(q > 0.0) || !isfinite(result->p_velocity_mps) || !isfinite(result->s_velocity_mps)) {
        result->status = LOCALIZATION_PSP_NONPHYSICAL_VELOCITIES;
        return ESP_OK;
    }

    double p_sum = 0.0;
    double s_sum = 0.0;
    for (size_t i = 0U; i < observation_count; ++i) {
        const double distance = ext_distance(position, observations[i].sensor_position);
        const double rp = problem.p_arrival_s[i] - params[problem.index_t0] - p * distance;
        p_sum += rp * rp;
        if (observations[i].s_valid) {
            const double rs = problem.s_minus_p_s[i] - q * distance;
            s_sum += rs * rs;
        }
    }
    result->normal_rcond = ext_normal_rcond(
        problem.param_count, 2U * observation_count + 1U, psp_row, &problem, params
    );
    result->status = result->normal_rcond < EXT_UNDERDETERMINED_RCOND
        ? LOCALIZATION_PSP_UNDERDETERMINED
        : LOCALIZATION_PSP_SOLVED;
    result->position = position;
    result->emission_time_us = earliest + (int64_t)llround(params[problem.index_t0] * 1.0e6);
    result->rms_p_residual_us = sqrt(p_sum / (double)observation_count) * 1.0e6;
    result->rms_s_minus_p_residual_us = sqrt(s_sum / (double)s_used) * 1.0e6;
    result->iterations = lm.iterations;
    result->converged = lm.converged;
    return ESP_OK;
}
