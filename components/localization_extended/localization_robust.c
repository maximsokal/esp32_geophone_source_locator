#include "localization_robust.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_ext_fit.h"


#define ROBUST_DEFAULT_LATTICE_POINTS 8000U
#define ROBUST_DEFAULT_ITERATIONS 20U
#define ROBUST_DEFAULT_INLIER_SIGMA 3.0
#define ROBUST_LM_ITERATIONS 60U


localization_robust_config_t localization_robust_default_config(void)
{
    localization_robust_config_t config;
    memset(&config, 0, sizeof(config));
    config.loss = LOCALIZATION_EXTENDED_LOSS_TUKEY;
    config.inlier_threshold_sigma = ROBUST_DEFAULT_INLIER_SIGMA;
    config.consensus = true;
    return config;
}


/* Arrival-time problem shared by every stage: t_i = t0 + slowness * D_i. */
typedef struct
{
    const localization_observation_t *observations;
    size_t count;
    double arrival_s[LOCALIZATION_EXTENDED_MAX_SENSORS]; /* relative to the earliest */
    double weight[LOCALIZATION_EXTENDED_MAX_SENSORS];    /* IRLS weight, 0 = excluded */
    double slowness;
    double sigma_s;
    bool planar;
    const localization_extended_bounds_t *bounds;

} arrival_problem_t;


static localization_point3d_t params_position(const arrival_problem_t *problem, const double *params)
{
    localization_point3d_t point = { params[0], params[1], problem->bounds->min_corner.z_m };
    if (!problem->planar) {
        point.z_m = params[2];
    }
    return point;
}


static size_t param_count(const arrival_problem_t *problem)
{
    return problem->planar ? 3U : 4U;
}


static bool arrival_row(size_t row, const double *params, double *residual, double *gradient, void *context)
{
    const arrival_problem_t *problem = context;
    if (!(problem->weight[row] > 0.0)) {
        return false;
    }
    const localization_point3d_t point = params_position(problem, params);
    const localization_point3d_t sensor = problem->observations[row].sensor_position;
    double distance = ext_distance(point, sensor);
    if (distance < 1.0e-9) {
        distance = 1.0e-9;
    }
    const size_t t0_index = param_count(problem) - 1U;
    const double scale = sqrt(problem->weight[row]) / problem->sigma_s;
    *residual = scale * (problem->arrival_s[row] - params[t0_index] - problem->slowness * distance);
    const double factor = -scale * problem->slowness / distance;
    gradient[0] = factor * (point.x_m - sensor.x_m);
    gradient[1] = factor * (point.y_m - sensor.y_m);
    if (!problem->planar) {
        gradient[2] = factor * (point.z_m - sensor.z_m);
    }
    gradient[t0_index] = -scale;
    return true;
}


static void arrival_project(double *params, void *context)
{
    const arrival_problem_t *problem = context;
    const localization_point3d_t clamped =
        ext_clamp_to_bounds(problem->bounds, params_position(problem, params));
    params[0] = clamped.x_m;
    params[1] = clamped.y_m;
    if (!problem->planar) {
        params[2] = clamped.z_m;
    }
}


/* Weighted-mean origin time at a position (the profiled t0). */
static double profiled_t0(const arrival_problem_t *problem, localization_point3d_t point)
{
    double sum = 0.0;
    double weight_sum = 0.0;
    for (size_t i = 0U; i < problem->count; ++i) {
        if (problem->weight[i] > 0.0) {
            sum += problem->weight[i] *
                (problem->arrival_s[i] - problem->slowness * ext_distance(point, problem->observations[i].sensor_position));
            weight_sum += problem->weight[i];
        }
    }
    return weight_sum > 0.0 ? sum / weight_sum : 0.0;
}


/* Best admitted lattice point for the current weights (profiled t0). */
static bool seed_from_lattice(
    const arrival_problem_t *problem,
    const ext_lattice_t *lattice,
    const uint8_t *admitted,
    localization_point3d_t *best_out
)
{
    const size_t total = ext_lattice_count(lattice);
    float best_cost = INFINITY;
    bool found = false;
    for (size_t index = 0U; index < total; ++index) {
        if (admitted[index] == 0U) {
            continue;
        }
        const localization_point3d_t point = ext_lattice_point(lattice, index);
        float residual[LOCALIZATION_EXTENDED_MAX_SENSORS];
        float sum = 0.0f;
        float weight_sum = 0.0f;
        for (size_t i = 0U; i < problem->count; ++i) {
            if (!(problem->weight[i] > 0.0)) {
                continue;
            }
            const float dx = (float)(point.x_m - problem->observations[i].sensor_position.x_m);
            const float dy = (float)(point.y_m - problem->observations[i].sensor_position.y_m);
            const float dz = (float)(point.z_m - problem->observations[i].sensor_position.z_m);
            residual[i] = (float)problem->arrival_s[i] - (float)problem->slowness * sqrtf(dx * dx + dy * dy + dz * dz);
            sum += (float)problem->weight[i] * residual[i];
            weight_sum += (float)problem->weight[i];
        }
        const float t0 = sum / weight_sum;
        float cost = 0.0f;
        for (size_t i = 0U; i < problem->count; ++i) {
            if (problem->weight[i] > 0.0) {
                const float r = residual[i] - t0;
                cost += (float)problem->weight[i] * r * r;
            }
        }
        if (cost < best_cost) {
            best_cost = cost;
            *best_out = point;
            found = true;
        }
    }
    return found;
}


/* Least-squares fit under the current weights, from `start`. Keeps `start`
 * when the refined point is not admitted by the candidate filter. */
static void fit_from(
    arrival_problem_t *problem,
    const localization_candidate_filter_t *filter,
    localization_point3d_t start,
    localization_point3d_t *position_out,
    double *t0_out
)
{
    double params[4] = { start.x_m, start.y_m, 0.0, 0.0 };
    const size_t count = param_count(problem);
    if (!problem->planar) {
        params[2] = start.z_m;
    }
    params[count - 1U] = profiled_t0(problem, start);
    const double scale[4] = { 1.0, 1.0, problem->planar ? 1.0e-3 : 1.0, 1.0e-3 };
    ext_lm_result_t lm;
    if (
        ext_lm(count, problem->count, arrival_row, arrival_project, problem,
               scale, 1.0e-7, ROBUST_LM_ITERATIONS, params, &lm)
    ) {
        const localization_point3d_t refined = params_position(problem, params);
        if (localization_candidate_admitted(filter, &refined)) {
            *position_out = refined;
            *t0_out = params[count - 1U];
            return;
        }
    }
    *position_out = start;
    *t0_out = profiled_t0(problem, start);
}


static double residual_s(const arrival_problem_t *problem, size_t i, localization_point3d_t point, double t0)
{
    return problem->arrival_s[i] - t0 - problem->slowness * ext_distance(point, problem->observations[i].sensor_position);
}


static size_t popcount(uint32_t bits)
{
    size_t count = 0U;
    for (; bits != 0U; bits &= bits - 1U) {
        ++count;
    }
    return count;
}


/* Median of (t_i - slowness * D_i) over `mask`: an origin time that one
 * bad pick cannot move. */
static double median_t0(const arrival_problem_t *problem, uint32_t mask, localization_point3d_t point)
{
    double values[LOCALIZATION_EXTENDED_MAX_SENSORS];
    size_t count = 0U;
    for (size_t i = 0U; i < problem->count; ++i) {
        if ((mask & (1U << i)) != 0U) {
            const double value = problem->arrival_s[i] -
                problem->slowness * ext_distance(point, problem->observations[i].sensor_position);
            size_t j = count++;
            for (; j > 0U && values[j - 1U] > value; --j) {
                values[j] = values[j - 1U];
            }
            values[j] = value;
        }
    }
    if (count == 0U) {
        return 0.0;
    }
    return (count % 2U) == 1U
        ? values[count / 2U]
        : 0.5 * (values[count / 2U - 1U] + values[count / 2U]);
}


/* Sets the IRLS weights from the residuals at (point, t0); returns the
 * largest weight change. */
static double reweight(
    arrival_problem_t *problem,
    localization_extended_loss_t loss,
    double k,
    uint32_t mask,
    localization_point3d_t point,
    double t0
)
{
    double change = 0.0;
    for (size_t i = 0U; i < problem->count; ++i) {
        double weight = 0.0;
        if ((mask & (1U << i)) != 0U) {
            const double u = residual_s(problem, i, point, t0) / problem->sigma_s;
            /* A tiny floor keeps a fully rejected pick from making the
             * weighted system singular; it carries no practical weight. */
            weight = localization_extended_robust_weight(loss, u, k);
            if (weight < 1.0e-6) {
                weight = 1.0e-6;
            }
        }
        change = fmax(change, fabs(weight - problem->weight[i]));
        problem->weight[i] = weight;
    }
    return change;
}


/* Iteratively reweighted fit over the observations in `mask`, starting
 * from `position` (weights from its residuals, with a median origin time,
 * so a good start is never traded for the unweighted fit). */
static size_t irls(
    arrival_problem_t *problem,
    const localization_robust_config_t *config,
    localization_extended_loss_t loss,
    double k,
    uint32_t mask,
    localization_point3d_t *position,
    double *t0,
    bool *iteration_limit_reached
)
{
    for (size_t i = 0U; i < problem->count; ++i) {
        problem->weight[i] = 0.0;
    }
    *t0 = median_t0(problem, mask, *position);
    (void)reweight(problem, loss, k, mask, *position, *t0);
    const size_t rounds = config->max_iterations > 0U ? config->max_iterations : ROBUST_DEFAULT_ITERATIONS;
    size_t round = 0U;
    bool converged = false;
    while (round < rounds) {
        fit_from(problem, &config->candidate_filter, *position, position, t0);
        ++round;
        if (reweight(problem, loss, k, mask, *position, *t0) < 1.0e-4) {
            converged = true;
            break;
        }
    }
    if (iteration_limit_reached != NULL) {
        *iteration_limit_reached = !converged && round >= rounds;
    }
    return round;
}


esp_err_t localization_robust_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_extended_bounds_t *bounds,
    const localization_robust_config_t *config,
    localization_robust_result_t *result
)
{
    if (
        observations == NULL || bounds == NULL || config == NULL || result == NULL ||
        observation_count == 0U || observation_count > LOCALIZATION_EXTENDED_MAX_SENSORS ||
        !localization_extended_bounds_valid(bounds) ||
        !(config->propagation_speed_mps > 0.0) || !isfinite(config->propagation_speed_mps) ||
        !(config->timing_uncertainty_us > 0.0) || !isfinite(config->timing_uncertainty_us)
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->planar = localization_extended_bounds_planar(bounds);

    const size_t minimal = result->planar ? 3U : 4U;
    if (observation_count < minimal) {
        result->status = LOCALIZATION_ROBUST_INSUFFICIENT_OBSERVATIONS;
        result->degrees_of_freedom = (int32_t)observation_count - (int32_t)minimal;
        return ESP_OK;
    }

    ext_lattice_t lattice;
    if (!ext_lattice_init(
            bounds,
            config->max_lattice_points > 0U ? config->max_lattice_points : ROBUST_DEFAULT_LATTICE_POINTS,
            &lattice
        )) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t lattice_count = ext_lattice_count(&lattice);
    uint8_t *admitted = malloc(lattice_count);
    if (admitted == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t admitted_count = 0U;
    for (size_t index = 0U; index < lattice_count; ++index) {
        const localization_point3d_t point = ext_lattice_point(&lattice, index);
        admitted[index] = localization_candidate_admitted(&config->candidate_filter, &point) ? 1U : 0U;
        admitted_count += admitted[index];
    }
    if (admitted_count == 0U) {
        free(admitted);
        result->status = LOCALIZATION_ROBUST_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }

    arrival_problem_t problem;
    memset(&problem, 0, sizeof(problem));
    problem.observations = observations;
    problem.count = observation_count;
    problem.slowness = 1.0 / config->propagation_speed_mps;
    problem.sigma_s = config->timing_uncertainty_us * 1.0e-6;
    problem.planar = result->planar;
    problem.bounds = bounds;
    int64_t earliest = observations[0].arrival_time_us;
    for (size_t i = 1U; i < observation_count; ++i) {
        if (observations[i].arrival_time_us < earliest) {
            earliest = observations[i].arrival_time_us;
        }
    }
    for (size_t i = 0U; i < observation_count; ++i) {
        problem.arrival_s[i] = (double)(observations[i].arrival_time_us - earliest) * 1.0e-6;
    }

    const double threshold = config->inlier_threshold_sigma > 0.0
        ? config->inlier_threshold_sigma : ROBUST_DEFAULT_INLIER_SIGMA;
    localization_extended_loss_t loss = config->loss;
    double k = config->tuning_k > 0.0
        ? config->tuning_k
        : (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY ? LOCALIZATION_EXTENDED_TUKEY_K : LOCALIZATION_EXTENDED_HUBER_K);

    const uint32_t all = (1U << observation_count) - 1U;
    uint32_t fit_mask = all;
    localization_point3d_t position = lattice.origin;

    if (config->consensus && observation_count > minimal) {
        /* Exhaustive MSAC over every minimal subset (at most C(8, 4) = 70). */
        result->consensus_run = true;
        double best_score = INFINITY;
        uint32_t best_inliers = 0U;
        localization_point3d_t best_position = lattice.origin;
        uint32_t masks_seen[70];
        size_t seen = 0U;

        for (uint32_t subset = 0U; subset <= all; ++subset) {
            if (popcount(subset) != minimal) {
                continue;
            }
            for (size_t i = 0U; i < observation_count; ++i) {
                problem.weight[i] = (subset & (1U << i)) != 0U ? 1.0 : 0.0;
            }
            localization_point3d_t seed;
            if (!seed_from_lattice(&problem, &lattice, admitted, &seed)) {
                continue;
            }
            localization_point3d_t fitted;
            double t0 = 0.0;
            fit_from(&problem, &config->candidate_filter, seed, &fitted, &t0);
            ++result->subsets_evaluated;

            double score = 0.0;
            uint32_t inliers = 0U;
            for (size_t i = 0U; i < observation_count; ++i) {
                const double u = residual_s(&problem, i, fitted, t0) / problem.sigma_s;
                score += fmin(u * u, threshold * threshold);
                if (fabs(u) <= threshold) {
                    inliers |= 1U << i;
                }
            }
            if (seen < 70U) {
                masks_seen[seen++] = inliers;
            }
            if (score < best_score) {
                best_score = score;
                best_inliers = inliers;
                best_position = fitted;
            }
        }

        /* Another subset reaching as large a consensus with different
         * members means the data cannot say which pick is wrong. */
        const size_t best_count = popcount(best_inliers);
        for (size_t s = 0U; s < seen; ++s) {
            if (popcount(masks_seen[s]) >= best_count && masks_seen[s] != best_inliers) {
                result->consensus_ambiguous = true;
            }
        }
        position = best_position;
        if (!result->consensus_ambiguous && best_count >= minimal) {
            fit_mask = best_inliers;
        } else {
            loss = LOCALIZATION_EXTENDED_LOSS_HUBER;
            k = LOCALIZATION_EXTENDED_HUBER_K;
        }
    } else {
        for (size_t i = 0U; i < observation_count; ++i) {
            problem.weight[i] = 1.0;
        }
        (void)seed_from_lattice(&problem, &lattice, admitted, &position);
        if (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY) {
            /* A redescending loss needs a start that is not already pulled
             * by the outlier: begin from the convex Huber fit. */
            double t0 = 0.0;
            (void)irls(&problem, config, LOCALIZATION_EXTENDED_LOSS_HUBER,
                       LOCALIZATION_EXTENDED_HUBER_K, all, &position, &t0, NULL);
        }
    }
    free(admitted);

    double t0 = 0.0;
    bool iteration_limit_reached = false;
    result->irls_iterations = irls(
        &problem, config, loss, k, fit_mask, &position, &t0, &iteration_limit_reached
    );

    /* Re-admit an excluded pick that the final fit explains after all. */
    uint32_t inliers = 0U;
    for (size_t i = 0U; i < observation_count; ++i) {
        const double u = residual_s(&problem, i, position, t0) / problem.sigma_s;
        if (fabs(u) <= threshold) {
            inliers |= 1U << i;
        }
    }
    if (result->consensus_run && !result->consensus_ambiguous) {
        if ((inliers & ~fit_mask) != 0U && popcount(inliers) >= minimal) {
            fit_mask |= inliers;
            result->irls_iterations += irls(
                &problem, config, loss, k, fit_mask, &position, &t0, &iteration_limit_reached
            );
        }
    } else {
        fit_mask = all;
    }
    result->iteration_limit_reached = iteration_limit_reached;

    result->status = LOCALIZATION_ROBUST_SOLVED;
    result->position = position;
    result->emission_time_us = earliest + (int64_t)llround(t0 * 1.0e6);
    double sum_squares = 0.0;
    uint32_t final_inliers = 0U;
    for (size_t i = 0U; i < observation_count; ++i) {
        const double r = residual_s(&problem, i, position, t0);
        result->residual_us[i] = r * 1.0e6;
        if ((fit_mask & (1U << i)) != 0U && fabs(r / problem.sigma_s) <= threshold) {
            final_inliers |= 1U << i;
        }
        if ((fit_mask & (1U << i)) != 0U) {
            sum_squares += r * r;
        }
    }
    /* When the consensus was ambiguous nothing is rejected: every pick
     * stays "used" and none is named an outlier. */
    if (!result->consensus_run || result->consensus_ambiguous) {
        final_inliers = all;
    }
    result->inlier_mask = final_inliers;
    result->outlier_mask = all & ~final_inliers;
    result->observations_used = popcount(fit_mask & final_inliers);
    result->rms_residual_us = sqrt(sum_squares / (double)popcount(fit_mask)) * 1.0e6;
    result->degrees_of_freedom = (int32_t)result->observations_used - (int32_t)minimal;
    return ESP_OK;
}
