#include "localization_bayes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_ext_fit.h"


#define BAYES_DEFAULT_NU 4.0
#define BAYES_DEFAULT_LEVEL 0.9
#define BAYES_DEFAULT_NODES 7U
#define BAYES_MAX_NODES 9U
#define BAYES_DEFAULT_CELLS 40000U
#define BAYES_STUDENT_ROUNDS 4U
/* Fewer credible cells than this and the lattice is zoomed in. */
#define BAYES_RESOLVED_CELLS 64U
#define BAYES_SUPPORT_LEVEL 0.999
#define BAYES_MAX_LEVELS 5U


/* Gauss-Hermite abscissae and weights (physicists', weight exp(-x^2)),
 * non-negative half; the rule is symmetric. */
static const double GH_X[5][5] = {
    { 0.0 },
    { 0.0, 1.224744871391589 },
    { 0.0, 0.958572464613819, 2.020182870456086 },
    { 0.0, 0.816287882858965, 1.673551628767471, 2.651961356835233 },
    { 0.0, 0.723551018752838, 1.468553289216668, 2.266580584531843, 3.190993201781528 },
};
static const double GH_W[5][5] = {
    { 1.772453850905516 },
    { 1.181635900603677, 0.295408975150919 },
    { 0.945308720482942, 0.393619323152241, 0.019953242059046 },
    { 0.810264617556807, 0.425607252610128, 0.054515582819127, 0.000971781245100 },
    { 0.720235215606051, 0.432651559002556, 0.088474527394377, 0.004943624275537, 0.000039606977263 },
};


typedef struct
{
    const localization_bayes_pick_t *picks;
    size_t count;
    const localization_bayes_config_t *config;
    double arrival_s[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double weight[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double sigma_s;
    double nu;
    size_t node_count;
    double velocity[BAYES_MAX_NODES];
    double log_node_weight[BAYES_MAX_NODES];

} bayes_problem_t;


/* Reference-velocity travel times of every used pick; false when one is
 * unreachable. */
static bool reference_times(const bayes_problem_t *problem, const localization_point3d_t *point, double *times)
{
    const localization_bayes_config_t *config = problem->config;
    for (size_t i = 0U; i < problem->count; ++i) {
        if (!(problem->weight[i] > 0.0)) {
            times[i] = 0.0;
            continue;
        }
        if (config->travel_time_model.travel_time != NULL) {
            if (
                !config->travel_time_model.travel_time(i, point, &times[i], config->travel_time_model.context) ||
                !isfinite(times[i])
            ) {
                return false;
            }
        } else {
            times[i] = ext_distance(*point, problem->picks[i].sensor_position) / config->velocity_mps;
        }
    }
    return true;
}


/* Log-likelihood at one velocity (times scaled from the reference), with
 * the origin time integrated out (Gaussian) or profiled (Student-t). */
static double log_likelihood(const bayes_problem_t *problem, const double *reference, double scale, double *t0_out)
{
    double r[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double sum = 0.0;
    double weight_sum = 0.0;
    for (size_t i = 0U; i < problem->count; ++i) {
        if (problem->weight[i] > 0.0) {
            r[i] = problem->arrival_s[i] - scale * reference[i];
            sum += problem->weight[i] * r[i];
            weight_sum += problem->weight[i];
        }
    }
    double t0 = sum / weight_sum;
    if (problem->config->likelihood == LOCALIZATION_BAYES_STUDENT_T) {
        for (size_t round = 0U; round < BAYES_STUDENT_ROUNDS; ++round) {
            double num = 0.0;
            double den = 0.0;
            for (size_t i = 0U; i < problem->count; ++i) {
                if (problem->weight[i] > 0.0) {
                    const double u2 = problem->weight[i] * (r[i] - t0) * (r[i] - t0) / (problem->sigma_s * problem->sigma_s);
                    const double w = problem->weight[i] * (problem->nu + 1.0) / (problem->nu + u2);
                    num += w * r[i];
                    den += w;
                }
            }
            t0 = num / den;
        }
        double log_l = 0.0;
        for (size_t i = 0U; i < problem->count; ++i) {
            if (problem->weight[i] > 0.0) {
                const double u2 = problem->weight[i] * (r[i] - t0) * (r[i] - t0) / (problem->sigma_s * problem->sigma_s);
                log_l -= 0.5 * (problem->nu + 1.0) * log1p(u2 / problem->nu);
            }
        }
        *t0_out = t0;
        return log_l;
    }
    double chi = 0.0;
    for (size_t i = 0U; i < problem->count; ++i) {
        if (problem->weight[i] > 0.0) {
            chi += problem->weight[i] * (r[i] - t0) * (r[i] - t0);
        }
    }
    *t0_out = t0;
    return -0.5 * chi / (problem->sigma_s * problem->sigma_s);
}


/* Log posterior (unnormalized) at a point, velocity marginalized; also the
 * posterior-weighted velocity moments at this point. -INFINITY outside
 * the prior's support or where a sensor is unreachable. */
static double log_posterior(
    const bayes_problem_t *problem,
    const localization_point3d_t *point,
    double *velocity_mean,
    double *velocity_second
)
{
    if (!localization_candidate_admitted(&problem->config->candidate_filter, point)) {
        return -INFINITY;
    }
    double reference[LOCALIZATION_EXTENDED_MAX_SENSORS];
    if (!reference_times(problem, point, reference)) {
        return -INFINITY;
    }
    double terms[BAYES_MAX_NODES];
    double peak = -INFINITY;
    for (size_t k = 0U; k < problem->node_count; ++k) {
        double t0 = 0.0;
        terms[k] = problem->log_node_weight[k] +
            log_likelihood(problem, reference, problem->config->velocity_mps / problem->velocity[k], &t0);
        peak = fmax(peak, terms[k]);
    }
    double total = 0.0;
    double first = 0.0;
    double second = 0.0;
    for (size_t k = 0U; k < problem->node_count; ++k) {
        const double w = exp(terms[k] - peak);
        total += w;
        first += w * problem->velocity[k];
        second += w * problem->velocity[k] * problem->velocity[k];
    }
    if (velocity_mean != NULL) {
        *velocity_mean = first / total;
        *velocity_second = second / total;
    }
    return peak + log(total);
}


static double hpd_threshold(const float *log_p, size_t cells, double peak, double total, double level)
{
    double low = -80.0;   /* relative log probability */
    double high = 0.0;
    for (int iteration = 0; iteration < 60; ++iteration) {
        const double middle = 0.5 * (low + high);
        double mass = 0.0;
        for (size_t index = 0U; index < cells; ++index) {
            const double relative = (double)log_p[index] - peak;
            if (isfinite(relative) && relative >= middle) {
                mass += exp(relative);
            }
        }
        if (mass / total >= level) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}


static void extend_box(localization_point3d_t *min, localization_point3d_t *max, localization_point3d_t point, bool *first)
{
    if (*first) {
        *min = point;
        *max = point;
        *first = false;
        return;
    }
    min->x_m = fmin(min->x_m, point.x_m);
    min->y_m = fmin(min->y_m, point.y_m);
    min->z_m = fmin(min->z_m, point.z_m);
    max->x_m = fmax(max->x_m, point.x_m);
    max->y_m = fmax(max->y_m, point.y_m);
    max->z_m = fmax(max->z_m, point.z_m);
}


/* Posterior over one lattice: MAP cell, mean, standard deviations,
 * velocity posterior and the highest-posterior-density region. Sets
 * result->status to SOLVED, NO_ALLOWED_CANDIDATE or
 * NO_REACHABLE_CANDIDATE. */
static esp_err_t evaluate_lattice(
    const bayes_problem_t *problem,
    const ext_lattice_t *lattice,
    localization_bayes_result_t *result,
    size_t *map_index_out,
    localization_point3d_t *support_min,
    localization_point3d_t *support_max
)
{
    const localization_bayes_config_t *config = problem->config;
    result->cell_m = lattice->step_m;
    result->credible_cells = 0U;
    const size_t cells = ext_lattice_count(lattice);
    float *log_p = malloc(cells * sizeof(float));
    if (log_p == NULL) {
        return ESP_ERR_NO_MEM;
    }

    double peak = -INFINITY;
    size_t map_index = 0U;
    bool any_admitted = false;
    for (size_t index = 0U; index < cells; ++index) {
        const localization_point3d_t point = ext_lattice_point(lattice, index);
        if (localization_candidate_admitted(&config->candidate_filter, &point)) {
            any_admitted = true;
        }
        const double lp = log_posterior(problem, &point, NULL, NULL);
        log_p[index] = (float)lp;
        ++result->cells_evaluated;
        if (lp > peak) {
            peak = lp;
            map_index = index;
        }
    }
    if (!any_admitted || !isfinite(peak)) {
        free(log_p);
        result->status = any_admitted
            ? LOCALIZATION_BAYES_NO_REACHABLE_CANDIDATE
            : LOCALIZATION_BAYES_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }
    *map_index_out = map_index;
    result->map_position = ext_lattice_point(lattice, map_index);

    /* Normalize; moments of position and velocity. */
    double total = 0.0;
    double mean[3] = { 0.0, 0.0, 0.0 };
    double v_first = 0.0;
    double v_second = 0.0;
    double v_mass = 0.0;
    for (size_t index = 0U; index < cells; ++index) {
        if (!isfinite(log_p[index])) {
            continue;
        }
        const double p = exp((double)log_p[index] - peak);
        const localization_point3d_t point = ext_lattice_point(lattice, index);
        total += p;
        mean[0] += p * point.x_m;
        mean[1] += p * point.y_m;
        mean[2] += p * point.z_m;
        if (p > 1.0e-6) {
            double vm = 0.0, vs = 0.0;
            (void)log_posterior(problem, &point, &vm, &vs);
            v_first += p * vm;
            v_second += p * vs;
            v_mass += p;
        }
    }
    for (size_t a = 0U; a < 3U; ++a) {
        mean[a] /= total;
    }
    double variance[3] = { 0.0, 0.0, 0.0 };
    for (size_t index = 0U; index < cells; ++index) {
        if (!isfinite(log_p[index])) {
            continue;
        }
        const double p = exp((double)log_p[index] - peak) / total;
        const localization_point3d_t point = ext_lattice_point(lattice, index);
        variance[0] += p * (point.x_m - mean[0]) * (point.x_m - mean[0]);
        variance[1] += p * (point.y_m - mean[1]) * (point.y_m - mean[1]);
        variance[2] += p * (point.z_m - mean[2]) * (point.z_m - mean[2]);
    }
    /* The lattice resolves nothing finer than a cell: add its uniform
     * quantization variance on every searched axis. */
    const double quantization = lattice->step_m * lattice->step_m / 12.0;
    result->mean_position.x_m = mean[0];
    result->mean_position.y_m = mean[1];
    result->mean_position.z_m = mean[2];
    result->std_m.x_m = sqrt(variance[0] + (lattice->nx > 1U ? quantization : 0.0));
    result->std_m.y_m = sqrt(variance[1] + (lattice->ny > 1U ? quantization : 0.0));
    result->std_m.z_m = sqrt(variance[2] + (lattice->nz > 1U ? quantization : 0.0));
    result->map_cell_probability = 1.0 / total;
    result->velocity_mean_mps = v_first / v_mass;
    const double v_var = v_second / v_mass - result->velocity_mean_mps * result->velocity_mean_mps;
    result->velocity_std_mps = v_var > 0.0 ? sqrt(v_var) : 0.0;

    /* Highest-posterior-density region: the probability threshold above
     * which `level` of the mass lies (by bisection on log p); the same at
     * BAYES_SUPPORT_LEVEL bounds where a finer lattice must look. */
    const double level = result->credible_level;
    const double threshold = hpd_threshold(log_p, cells, peak, total, level);
    const double support = hpd_threshold(log_p, cells, peak, total, BAYES_SUPPORT_LEVEL);
    bool first_cell = true;
    bool first_support = true;
    for (size_t index = 0U; index < cells; ++index) {
        const double relative = (double)log_p[index] - peak;
        if (!isfinite(relative) || relative < support) {
            continue;
        }
        const localization_point3d_t point = ext_lattice_point(lattice, index);
        extend_box(support_min, support_max, point, &first_support);
        if (relative < threshold) {
            continue;
        }
        extend_box(&result->credible_min, &result->credible_max, point, &first_cell);
        ++result->credible_cells;
    }
    const double cell_area = lattice->step_m * lattice->step_m;
    result->credible_area_m2 = 0.0;
    result->credible_volume_m3 = 0.0;
    if (result->planar) {
        result->credible_area_m2 = (double)result->credible_cells * cell_area;
    } else {
        result->credible_volume_m3 = (double)result->credible_cells * cell_area * lattice->step_m;
    }
    free(log_p);
    result->status = LOCALIZATION_BAYES_SOLVED;
    return ESP_OK;
}


esp_err_t localization_bayes_locate(
    const localization_bayes_pick_t *picks,
    size_t pick_count,
    const localization_extended_bounds_t *bounds,
    const localization_bayes_config_t *config,
    localization_bayes_result_t *result
)
{
    if (
        picks == NULL || bounds == NULL || config == NULL || result == NULL ||
        pick_count == 0U || pick_count > LOCALIZATION_EXTENDED_MAX_SENSORS ||
        !localization_extended_bounds_valid(bounds) ||
        !(config->velocity_mps > 0.0) || !isfinite(config->velocity_mps) ||
        !(config->velocity_sigma_mps >= 0.0) || !isfinite(config->velocity_sigma_mps) ||
        !(config->timing_uncertainty_us > 0.0) || !isfinite(config->timing_uncertainty_us) ||
        config->credible_level < 0.0 || config->credible_level >= 1.0 ||
        config->velocity_nodes > BAYES_MAX_NODES ||
        (config->velocity_nodes != 0U && (config->velocity_nodes % 2U) == 0U) ||
        config->refine_divisor < 2U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->planar = localization_extended_bounds_planar(bounds);
    result->credible_level = config->credible_level > 0.0 ? config->credible_level : BAYES_DEFAULT_LEVEL;

    bayes_problem_t problem;
    memset(&problem, 0, sizeof(problem));
    problem.picks = picks;
    problem.count = pick_count;
    problem.config = config;
    problem.sigma_s = config->timing_uncertainty_us * 1.0e-6;
    problem.nu = config->student_nu > 0.0 ? config->student_nu : BAYES_DEFAULT_NU;
    int64_t earliest = INT64_MAX;
    for (size_t i = 0U; i < pick_count; ++i) {
        if (!(picks[i].weight >= 0.0) || !isfinite(picks[i].weight)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (picks[i].weight > 0.0) {
            earliest = picks[i].arrival_time_us < earliest ? picks[i].arrival_time_us : earliest;
            result->pick_mask_used |= 1U << i;
            ++result->picks_used;
        }
    }
    const size_t spatial = result->planar ? 2U : 3U;
    if (result->picks_used < spatial + 1U) {
        result->status = LOCALIZATION_BAYES_INSUFFICIENT_PICKS;
        return ESP_OK;
    }
    for (size_t i = 0U; i < pick_count; ++i) {
        if (picks[i].weight > 0.0) {
            problem.weight[i] = picks[i].weight;
            problem.arrival_s[i] = (double)(picks[i].arrival_time_us - earliest) * 1.0e-6;
        }
    }

    /* Velocity quadrature: nodes of N(v, sigma), positive ones only. */
    const size_t requested = config->velocity_sigma_mps > 0.0
        ? (config->velocity_nodes > 0U ? config->velocity_nodes : BAYES_DEFAULT_NODES)
        : 1U;
    const size_t half = requested / 2U;
    for (long k = -(long)half; k <= (long)half; ++k) {
        const size_t a = (size_t)labs(k);
        const double x = (k < 0 ? -1.0 : 1.0) * GH_X[half][a];
        const double v = config->velocity_mps + sqrt(2.0) * config->velocity_sigma_mps * x;
        if (v > 0.0) {
            problem.velocity[problem.node_count] = v;
            problem.log_node_weight[problem.node_count] = log(GH_W[half][a] / sqrt(3.141592653589793));
            ++problem.node_count;
        }
    }

    /*
     * Multi-level evaluation. A pick-time posterior is often far narrower
     * than any affordable lattice (a 20 us pick at 300 m/s is 6 mm) and
     * can be a long thin ridge (depth under a flat array), which a coarse
     * lattice samples only by luck. Each level therefore uses a likelihood
     * tempered to its own resolution -- sigma no smaller than half a
     * cell's travel time -- so the ridge is seen, and the next level zooms
     * onto that level's BAYES_SUPPORT_LEVEL region at refine_divisor x
     * the resolution. The last level's sigma is reported; when the budget
     * stops the zoom before the true sigma is reached, the region is wider
     * (conservative), never narrower.
     */
    const size_t max_cells = config->max_cells > 0U ? config->max_cells : BAYES_DEFAULT_CELLS;
    localization_extended_bounds_t level_bounds = *bounds;
    ext_lattice_t lattice;
    size_t map_index = 0U;
    size_t evaluated = 0U;
    bool have_level = false;
    const double sigma = problem.sigma_s;
    double slowest = 1.0 / config->velocity_mps;
    for (size_t k = 0U; k < problem.node_count; ++k) {
        slowest = fmax(slowest, 1.0 / problem.velocity[k]);
    }
    for (size_t level = 0U; level < BAYES_MAX_LEVELS; ++level) {
        ext_lattice_t candidate;
        if (!ext_lattice_init(&level_bounds, max_cells, &candidate)) {
            return level == 0U ? ESP_ERR_INVALID_ARG : ESP_OK;
        }
        if (have_level && !(candidate.step_m < lattice.step_m)) {
            break; /* the budget allows no finer lattice */
        }
        problem.sigma_s = fmax(sigma, 0.5 * candidate.step_m * slowest);
        localization_bayes_result_t level_result = *result;
        level_result.cells_evaluated = 0U;
        localization_point3d_t support_min = level_bounds.min_corner;
        localization_point3d_t support_max = level_bounds.max_corner;
        size_t level_map = 0U;
        const esp_err_t err = evaluate_lattice(&problem, &candidate, &level_result, &level_map, &support_min, &support_max);
        if (err != ESP_OK) {
            return err;
        }
        evaluated += level_result.cells_evaluated;
        if (level_result.status != LOCALIZATION_BAYES_SOLVED) {
            if (!have_level) {
                result->status = level_result.status;
                result->cells_evaluated = evaluated;
                return ESP_OK;
            }
            break;
        }
        *result = level_result;
        result->timing_uncertainty_used_us = problem.sigma_s * 1.0e6;
        lattice = candidate;
        map_index = level_map;
        have_level = true;
        if (problem.sigma_s <= sigma && result->credible_cells >= BAYES_RESOLVED_CELLS) {
            break;
        }
        const double margin = candidate.step_m;
        level_bounds.min_corner = ext_clamp_to_bounds(bounds, (localization_point3d_t){
            support_min.x_m - margin, support_min.y_m - margin, support_min.z_m - margin,
        });
        level_bounds.max_corner = ext_clamp_to_bounds(bounds, (localization_point3d_t){
            support_max.x_m + margin, support_max.y_m + margin, support_max.z_m + margin,
        });
        level_bounds.step_m = candidate.step_m / (double)config->refine_divisor;
        if (result->planar) {
            level_bounds.min_corner.z_m = bounds->min_corner.z_m;
            level_bounds.max_corner.z_m = bounds->min_corner.z_m;
        }
    }
    result->cells_evaluated = evaluated;

    /* MAP refined on a finer local lattice (continuous log posterior). */
    localization_point3d_t best = ext_lattice_point(&lattice, map_index);
    double best_lp = log_posterior(&problem, &best, NULL, NULL);
    {
        const localization_point3d_t centre = best;
        const double step = lattice.step_m / (double)config->refine_divisor;
        const long span = (long)config->refine_divisor;
        const long span_z = result->planar ? 0 : span;
        for (long dz = -span_z; dz <= span_z; ++dz) {
            for (long dy = -span; dy <= span; ++dy) {
                for (long dx = -span; dx <= span; ++dx) {
                    const localization_point3d_t point = ext_clamp_to_bounds(bounds, (localization_point3d_t){
                        centre.x_m + (double)dx * step,
                        centre.y_m + (double)dy * step,
                        centre.z_m + (double)dz * step,
                    });
                    const double lp = log_posterior(&problem, &point, NULL, NULL);
                    ++result->cells_evaluated;
                    if (lp > best_lp) {
                        best_lp = lp;
                        best = point;
                    }
                }
            }
        }
    }
    result->map_position = best;

    /* Origin time and residuals at the MAP, posterior-mean velocity. */
    double reference[LOCALIZATION_EXTENDED_MAX_SENSORS];
    if (reference_times(&problem, &best, reference)) {
        double t0 = 0.0;
        (void)log_likelihood(&problem, reference, config->velocity_mps / result->velocity_mean_mps, &t0);
        double sum = 0.0;
        for (size_t i = 0U; i < pick_count; ++i) {
            if (problem.weight[i] > 0.0) {
                const double r = problem.arrival_s[i] - t0 - reference[i] * config->velocity_mps / result->velocity_mean_mps;
                sum += r * r;
            }
        }
        result->emission_time_us = earliest + (int64_t)llround(t0 * 1.0e6);
        result->rms_residual_us = sqrt(sum / (double)result->picks_used) * 1.0e6;
    }
    result->status = LOCALIZATION_BAYES_SOLVED;
    return ESP_OK;
}
