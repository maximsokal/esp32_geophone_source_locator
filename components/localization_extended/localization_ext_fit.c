#include "localization_ext_fit.h"

#include <math.h>
#include <string.h>


bool localization_extended_bounds_valid(const localization_extended_bounds_t *bounds)
{
    return
        bounds != NULL &&
        isfinite(bounds->step_m) && bounds->step_m > 0.0 &&
        isfinite(bounds->min_corner.x_m) && isfinite(bounds->max_corner.x_m) &&
        isfinite(bounds->min_corner.y_m) && isfinite(bounds->max_corner.y_m) &&
        isfinite(bounds->min_corner.z_m) && isfinite(bounds->max_corner.z_m) &&
        bounds->min_corner.x_m <= bounds->max_corner.x_m &&
        bounds->min_corner.y_m <= bounds->max_corner.y_m &&
        bounds->min_corner.z_m <= bounds->max_corner.z_m;
}


double localization_extended_robust_weight(localization_extended_loss_t loss, double u, double k)
{
    const double a = fabs(u);
    if (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY) {
        if (a >= k) {
            return 0.0;
        }
        const double t = 1.0 - (u / k) * (u / k);
        return t * t;
    }
    return a <= k ? 1.0 : k / a;
}


double localization_extended_robust_rho(localization_extended_loss_t loss, double u, double k)
{
    const double a = fabs(u);
    if (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY) {
        if (a >= k) {
            return k * k / 6.0;
        }
        const double t = 1.0 - (u / k) * (u / k);
        return k * k / 6.0 * (1.0 - t * t * t);
    }
    return a <= k ? 0.5 * u * u : k * (a - 0.5 * k);
}


static size_t axis_points(double min, double max, double step)
{
    if (max <= min) {
        return 1U;
    }
    return (size_t)floor((max - min) / step + 1.0e-9) + 1U;
}


bool ext_lattice_init(
    const localization_extended_bounds_t *bounds,
    size_t max_points,
    ext_lattice_t *lattice
)
{
    if (!localization_extended_bounds_valid(bounds) || lattice == NULL) {
        return false;
    }
    if (max_points == 0U) {
        max_points = EXT_DEFAULT_MAX_LATTICE_POINTS;
    }
    for (size_t factor = 1U; factor <= 4096U; ++factor) {
        const double step = bounds->step_m * (double)factor;
        const size_t nx = axis_points(bounds->min_corner.x_m, bounds->max_corner.x_m, step);
        const size_t ny = axis_points(bounds->min_corner.y_m, bounds->max_corner.y_m, step);
        const size_t nz = axis_points(bounds->min_corner.z_m, bounds->max_corner.z_m, step);
        if ((double)nx * (double)ny * (double)nz <= (double)max_points) {
            lattice->origin = bounds->min_corner;
            lattice->step_m = step;
            lattice->nx = nx;
            lattice->ny = ny;
            lattice->nz = nz;
            return true;
        }
    }
    return false;
}


size_t ext_lattice_count(const ext_lattice_t *lattice)
{
    return lattice->nx * lattice->ny * lattice->nz;
}


localization_point3d_t ext_lattice_point(const ext_lattice_t *lattice, size_t index)
{
    const size_t ix = index % lattice->nx;
    const size_t iy = (index / lattice->nx) % lattice->ny;
    const size_t iz = index / (lattice->nx * lattice->ny);
    const localization_point3d_t point = {
        lattice->origin.x_m + (double)ix * lattice->step_m,
        lattice->origin.y_m + (double)iy * lattice->step_m,
        lattice->origin.z_m + (double)iz * lattice->step_m,
    };
    return point;
}


static double clamp(double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}


localization_point3d_t ext_clamp_to_bounds(
    const localization_extended_bounds_t *bounds,
    localization_point3d_t point
)
{
    point.x_m = clamp(point.x_m, bounds->min_corner.x_m, bounds->max_corner.x_m);
    point.y_m = clamp(point.y_m, bounds->min_corner.y_m, bounds->max_corner.y_m);
    point.z_m = clamp(point.z_m, bounds->min_corner.z_m, bounds->max_corner.z_m);
    return point;
}


double ext_distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


bool ext_solve_spd(const double *a, const double *b, size_t n, double *x)
{
    if (a == NULL || b == NULL || x == NULL || n == 0U || n > EXT_MAX_PARAMS) {
        return false;
    }
    double l[EXT_MAX_PARAMS][EXT_MAX_PARAMS];
    memset(l, 0, sizeof(l));
    for (size_t i = 0U; i < n; ++i) {
        for (size_t j = 0U; j <= i; ++j) {
            double sum = a[i * n + j];
            for (size_t k = 0U; k < j; ++k) {
                sum -= l[i][k] * l[j][k];
            }
            if (i == j) {
                if (!(sum > 0.0) || !isfinite(sum)) {
                    return false;
                }
                l[i][i] = sqrt(sum);
            } else {
                l[i][j] = sum / l[j][j];
            }
        }
    }
    double y[EXT_MAX_PARAMS];
    for (size_t i = 0U; i < n; ++i) {
        double sum = b[i];
        for (size_t k = 0U; k < i; ++k) {
            sum -= l[i][k] * y[k];
        }
        y[i] = sum / l[i][i];
    }
    for (size_t i = n; i-- > 0U;) {
        double sum = y[i];
        for (size_t k = i + 1U; k < n; ++k) {
            sum -= l[k][i] * x[k];
        }
        x[i] = sum / l[i][i];
        if (!isfinite(x[i])) {
            return false;
        }
    }
    return true;
}


double ext_cost(
    size_t param_count,
    size_t row_count,
    ext_row_fn row,
    void *context,
    const double *params
)
{
    double gradient[EXT_MAX_PARAMS];
    double cost = 0.0;
    for (size_t r = 0U; r < row_count; ++r) {
        double residual = 0.0;
        memset(gradient, 0, sizeof(gradient[0]) * param_count);
        if (row(r, params, &residual, gradient, context)) {
            cost += residual * residual;
        }
    }
    return cost;
}


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
)
{
    if (
        param_count == 0U || param_count > EXT_MAX_PARAMS || row == NULL ||
        scale == NULL || params == NULL || result == NULL || !(tolerance > 0.0)
    ) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    double cost = ext_cost(param_count, row_count, row, context, params);
    if (!isfinite(cost)) {
        return false;
    }
    double lambda = 1.0e-3;

    for (size_t iteration = 0U; iteration < max_iterations; ++iteration) {
        double jtj[EXT_MAX_PARAMS * EXT_MAX_PARAMS];
        double jtr[EXT_MAX_PARAMS];
        double gradient[EXT_MAX_PARAMS];
        memset(jtj, 0, sizeof(jtj));
        memset(jtr, 0, sizeof(jtr));
        for (size_t r = 0U; r < row_count; ++r) {
            double residual = 0.0;
            memset(gradient, 0, sizeof(gradient));
            if (!row(r, params, &residual, gradient, context)) {
                continue;
            }
            for (size_t i = 0U; i < param_count; ++i) {
                jtr[i] += gradient[i] * residual;
                for (size_t j = 0U; j < param_count; ++j) {
                    jtj[i * param_count + j] += gradient[i] * gradient[j];
                }
            }
        }

        bool improved = false;
        double step[EXT_MAX_PARAMS];
        for (int attempt = 0; attempt < 12 && !improved; ++attempt) {
            double damped[EXT_MAX_PARAMS * EXT_MAX_PARAMS];
            double rhs[EXT_MAX_PARAMS];
            memcpy(damped, jtj, sizeof(damped));
            for (size_t i = 0U; i < param_count; ++i) {
                const double diagonal = jtj[i * param_count + i];
                damped[i * param_count + i] = diagonal + lambda * (diagonal > 0.0 ? diagonal : 1.0);
                rhs[i] = -jtr[i];
            }
            if (!ext_solve_spd(damped, rhs, param_count, step)) {
                lambda *= 10.0;
                continue;
            }
            double trial[EXT_MAX_PARAMS];
            for (size_t i = 0U; i < param_count; ++i) {
                trial[i] = params[i] + step[i];
            }
            if (project != NULL) {
                project(trial, context);
            }
            const double trial_cost = ext_cost(param_count, row_count, row, context, trial);
            if (isfinite(trial_cost) && trial_cost <= cost) {
                for (size_t i = 0U; i < param_count; ++i) {
                    step[i] = trial[i] - params[i];
                    params[i] = trial[i];
                }
                const double previous = cost;
                cost = trial_cost;
                lambda = lambda / 3.0 > 1.0e-12 ? lambda / 3.0 : 1.0e-12;
                improved = true;

                bool small = true;
                for (size_t i = 0U; i < param_count; ++i) {
                    if (fabs(step[i]) >= tolerance * scale[i]) {
                        small = false;
                    }
                }
                result->iterations = iteration + 1U;
                if (small || previous - cost <= 1.0e-14 * (previous + 1.0e-300)) {
                    result->converged = true;
                    result->cost = cost;
                    return true;
                }
            } else {
                lambda *= 4.0;
            }
        }
        result->iterations = iteration + 1U;
        if (!improved) {
            /* No descent step exists at any damping: a (local) minimum. */
            result->converged = true;
            break;
        }
    }
    result->cost = cost;
    return true;
}


/* Eigenvalues of a symmetric n x n matrix (n <= EXT_MAX_PARAMS), cyclic
 * Jacobi. The matrix is overwritten. */
static void symmetric_eigenvalues(double *a, size_t n, double *eigenvalues)
{
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (size_t p = 0U; p < n; ++p) {
            for (size_t q = p + 1U; q < n; ++q) {
                off += a[p * n + q] * a[p * n + q];
            }
        }
        if (off < 1.0e-30) {
            break;
        }
        for (size_t p = 0U; p < n; ++p) {
            for (size_t q = p + 1U; q < n; ++q) {
                const double apq = a[p * n + q];
                if (fabs(apq) < 1.0e-300) {
                    continue;
                }
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                const double c = 1.0 / sqrt(t * t + 1.0);
                const double s = t * c;
                for (size_t k = 0U; k < n; ++k) {
                    const double akp = a[k * n + p];
                    const double akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (size_t k = 0U; k < n; ++k) {
                    const double apk = a[p * n + k];
                    const double aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
            }
        }
    }
    for (size_t i = 0U; i < n; ++i) {
        eigenvalues[i] = a[i * n + i];
    }
}


double ext_normal_rcond(
    size_t param_count,
    size_t row_count,
    ext_row_fn row,
    void *context,
    const double *params
)
{
    if (param_count == 0U || param_count > EXT_MAX_PARAMS || row == NULL || params == NULL) {
        return 0.0;
    }
    double jtj[EXT_MAX_PARAMS * EXT_MAX_PARAMS];
    double gradient[EXT_MAX_PARAMS];
    memset(jtj, 0, sizeof(jtj));
    for (size_t r = 0U; r < row_count; ++r) {
        double residual = 0.0;
        memset(gradient, 0, sizeof(gradient));
        if (!row(r, params, &residual, gradient, context)) {
            continue;
        }
        for (size_t i = 0U; i < param_count; ++i) {
            for (size_t j = 0U; j < param_count; ++j) {
                jtj[i * param_count + j] += gradient[i] * gradient[j];
            }
        }
    }
    double scale[EXT_MAX_PARAMS];
    for (size_t i = 0U; i < param_count; ++i) {
        const double diagonal = jtj[i * param_count + i];
        if (!(diagonal > 0.0) || !isfinite(diagonal)) {
            return 0.0;
        }
        scale[i] = 1.0 / sqrt(diagonal);
    }
    for (size_t i = 0U; i < param_count; ++i) {
        for (size_t j = 0U; j < param_count; ++j) {
            jtj[i * param_count + j] *= scale[i] * scale[j];
        }
    }
    double eigenvalues[EXT_MAX_PARAMS];
    symmetric_eigenvalues(jtj, param_count, eigenvalues);
    double low = INFINITY;
    double high = 0.0;
    for (size_t i = 0U; i < param_count; ++i) {
        low = fmin(low, eigenvalues[i]);
        high = fmax(high, eigenvalues[i]);
    }
    return high > 0.0 && low > 0.0 ? low / high : 0.0;
}
