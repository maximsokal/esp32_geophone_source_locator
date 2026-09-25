#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "localization_extended.h"

/*
 * Private numerical core of localization_extended: a search lattice over a
 * localization_extended_bounds_t, a small SPD solver and a row-callback
 * Levenberg-Marquardt. Allocation-free; stack use is bounded by
 * EXT_MAX_PARAMS (the largest fit is PSP's x, y, z, t0, p, q), which keeps
 * the frames small enough for the 8 KiB HTTPS task that runs "Calculate
 * source".
 */

#define EXT_MAX_PARAMS 6U

/* Default lattice budget: a larger requested lattice gets a proportionally
 * larger step (by the smallest integer factor that fits). */
#define EXT_DEFAULT_MAX_LATTICE_POINTS 40000U


typedef struct
{
    localization_point3d_t origin;
    double step_m;
    size_t nx;
    size_t ny;
    size_t nz;

} ext_lattice_t;

bool ext_lattice_init(
    const localization_extended_bounds_t *bounds,
    size_t max_points,
    ext_lattice_t *lattice
);

size_t ext_lattice_count(const ext_lattice_t *lattice);

localization_point3d_t ext_lattice_point(const ext_lattice_t *lattice, size_t index);

localization_point3d_t ext_clamp_to_bounds(
    const localization_extended_bounds_t *bounds,
    localization_point3d_t point
);

double ext_distance(localization_point3d_t a, localization_point3d_t b);


/* Solves the symmetric positive definite n x n system (n <= EXT_MAX_PARAMS)
 * by Cholesky. False when a is not numerically positive definite. */
bool ext_solve_spd(const double *a, const double *b, size_t n, double *x);


/*
 * One whitened residual row of a least-squares problem: *residual and
 * gradient[k] = d residual / d params[k]. Returns false for an inactive row
 * (it contributes nothing).
 */
typedef bool (*ext_row_fn)(
    size_t row,
    const double *params,
    double *residual,
    double *gradient,
    void *context
);

/* Optional: maps params back into their feasible set (e.g. the box). */
typedef void (*ext_project_fn)(double *params, void *context);

typedef struct
{
    size_t iterations;
    bool converged;
    double cost; /* sum of squared residual rows at the solution */

} ext_lm_result_t;

/*
 * Minimizes sum_rows residual^2 over params by Levenberg-Marquardt
 * (Marquardt diagonal scaling). Convergence: every |step_k| <
 * tolerance * scale[k]. False only for invalid input or a non-finite
 * start; a run that stops at max_iterations returns true with
 * converged = false.
 */
bool ext_lm(
    size_t param_count,
    size_t row_count,
    ext_row_fn row,
    ext_project_fn project,
    void *context,
    const double *scale,
    double tolerance,
    size_t max_iterations,
    double *params,
    ext_lm_result_t *result
);

/* Sum of squared residual rows at params. */
double ext_cost(
    size_t param_count,
    size_t row_count,
    ext_row_fn row,
    void *context,
    const double *params
);

/*
 * Identifiability of a fitted least-squares problem: the ratio of the
 * smallest to the largest eigenvalue of the column-equilibrated normal
 * matrix J^T J at params (0 for a singular one). A ratio near 0 means a
 * direction in parameter space the data do not constrain -- a family of
 * equally good solutions, not one answer.
 */
double ext_normal_rcond(
    size_t param_count,
    size_t row_count,
    ext_row_fn row,
    void *context,
    const double *params
);

/* Below this rcond a fit is reported as underdetermined (a singular value
 * ratio of 1e-6). */
#define EXT_UNDERDETERMINED_RCOND 1.0e-12
