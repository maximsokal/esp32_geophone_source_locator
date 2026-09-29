#include "localization_vf_math.h"

#include <float.h>
#include <math.h>
#include <string.h>


#define VF_MAX_LOGISTIC_SAMPLES 64U


double vf_distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


bool vf_least_squares(
    const double *a,
    const double *b,
    size_t rows,
    size_t cols,
    double rank_tolerance,
    double *x
)
{
    if (
        a == NULL || b == NULL || x == NULL ||
        cols == 0U || cols > VF_MAX_COLS ||
        rows < cols || rows > VF_MAX_ROWS ||
        !(rank_tolerance > 0.0)
    ) {
        return false;
    }

    double m[VF_MAX_ROWS][VF_MAX_COLS];
    double rhs[VF_MAX_ROWS];
    double column_scale[VF_MAX_COLS];

    for (size_t j = 0U; j < cols; ++j) {
        double norm = 0.0;
        for (size_t i = 0U; i < rows; ++i) {
            norm += a[i * cols + j] * a[i * cols + j];
        }
        norm = sqrt(norm);
        if (!(norm > 0.0) || !isfinite(norm)) {
            return false;
        }
        column_scale[j] = norm;
    }

    for (size_t i = 0U; i < rows; ++i) {
        for (size_t j = 0U; j < cols; ++j) {
            m[i][j] = a[i * cols + j] / column_scale[j];
        }
        rhs[i] = b[i];
        if (!isfinite(rhs[i])) {
            return false;
        }
    }

    /* Householder QR: after step k, column k is zero below the diagonal. */
    for (size_t k = 0U; k < cols; ++k) {
        double norm = 0.0;
        for (size_t i = k; i < rows; ++i) {
            norm += m[i][k] * m[i][k];
        }
        norm = sqrt(norm);
        if (!(norm > 0.0)) {
            return false;
        }

        const double alpha = m[k][k] > 0.0 ? -norm : norm;
        double v[VF_MAX_ROWS];
        double v_norm_sq = 0.0;
        for (size_t i = k; i < rows; ++i) {
            v[i] = m[i][k];
        }
        v[k] -= alpha;
        for (size_t i = k; i < rows; ++i) {
            v_norm_sq += v[i] * v[i];
        }
        if (!(v_norm_sq > 0.0)) {
            continue;
        }

        for (size_t j = k; j < cols; ++j) {
            double dot = 0.0;
            for (size_t i = k; i < rows; ++i) {
                dot += v[i] * m[i][j];
            }
            const double factor = 2.0 * dot / v_norm_sq;
            for (size_t i = k; i < rows; ++i) {
                m[i][j] -= factor * v[i];
            }
        }

        double dot = 0.0;
        for (size_t i = k; i < rows; ++i) {
            dot += v[i] * rhs[i];
        }
        const double factor = 2.0 * dot / v_norm_sq;
        for (size_t i = k; i < rows; ++i) {
            rhs[i] -= factor * v[i];
        }
    }

    double max_diagonal = 0.0;
    for (size_t k = 0U; k < cols; ++k) {
        if (fabs(m[k][k]) > max_diagonal) {
            max_diagonal = fabs(m[k][k]);
        }
    }
    for (size_t k = 0U; k < cols; ++k) {
        if (!(fabs(m[k][k]) > rank_tolerance * max_diagonal)) {
            return false;
        }
    }

    double y[VF_MAX_COLS];
    for (size_t k = cols; k-- > 0U;) {
        double sum = rhs[k];
        for (size_t j = k + 1U; j < cols; ++j) {
            sum -= m[k][j] * y[j];
        }
        y[k] = sum / m[k][k];
    }

    for (size_t j = 0U; j < cols; ++j) {
        x[j] = y[j] / column_scale[j];
        if (!isfinite(x[j])) {
            return false;
        }
    }
    return true;
}


bool vf_solve_square(
    const double *a,
    const double *b,
    size_t n,
    double *x
)
{
    if (a == NULL || b == NULL || x == NULL || n == 0U || n > VF_MAX_COLS) {
        return false;
    }

    double m[VF_MAX_COLS][VF_MAX_COLS + 1U];
    double largest = 0.0;
    for (size_t i = 0U; i < n; ++i) {
        for (size_t j = 0U; j < n; ++j) {
            m[i][j] = a[i * n + j];
            if (fabs(m[i][j]) > largest) {
                largest = fabs(m[i][j]);
            }
        }
        m[i][n] = b[i];
    }
    if (!(largest > 0.0) || !isfinite(largest)) {
        return false;
    }

    for (size_t pivot = 0U; pivot < n; ++pivot) {
        size_t best = pivot;
        for (size_t row = pivot + 1U; row < n; ++row) {
            if (fabs(m[row][pivot]) > fabs(m[best][pivot])) {
                best = row;
            }
        }
        if (!(fabs(m[best][pivot]) > 1.0e-14 * largest)) {
            return false;
        }
        if (best != pivot) {
            for (size_t column = 0U; column <= n; ++column) {
                const double swap = m[pivot][column];
                m[pivot][column] = m[best][column];
                m[best][column] = swap;
            }
        }
        for (size_t row = pivot + 1U; row < n; ++row) {
            const double factor = m[row][pivot] / m[pivot][pivot];
            for (size_t column = pivot; column <= n; ++column) {
                m[row][column] -= factor * m[pivot][column];
            }
        }
    }

    for (size_t row = n; row-- > 0U;) {
        double sum = m[row][n];
        for (size_t column = row + 1U; column < n; ++column) {
            sum -= m[row][column] * x[column];
        }
        x[row] = sum / m[row][row];
        if (!isfinite(x[row])) {
            return false;
        }
    }
    return true;
}


static void jacobi_eigen_3x3(double a[3][3], double vectors[3][3])
{
    for (size_t i = 0U; i < 3U; ++i) {
        for (size_t j = 0U; j < 3U; ++j) {
            vectors[i][j] = i == j ? 1.0 : 0.0;
        }
    }

    for (int sweep = 0; sweep < 64; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        const double diagonal = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        if (off <= 1.0e-30 * (diagonal + DBL_MIN)) {
            break;
        }

        for (size_t p = 0U; p < 2U; ++p) {
            for (size_t q = p + 1U; q < 3U; ++q) {
                if (fabs(a[p][q]) <= DBL_MIN) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                const double c = 1.0 / sqrt(t * t + 1.0);
                const double s = t * c;

                for (size_t k = 0U; k < 3U; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (size_t k = 0U; k < 3U; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (size_t k = 0U; k < 3U; ++k) {
                    const double vkp = vectors[k][p];
                    const double vkq = vectors[k][q];
                    vectors[k][p] = c * vkp - s * vkq;
                    vectors[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
}


static localization_point3d_t cross(localization_point3d_t a, localization_point3d_t b)
{
    return (localization_point3d_t) {
        a.y_m * b.z_m - a.z_m * b.y_m,
        a.z_m * b.x_m - a.x_m * b.z_m,
        a.x_m * b.y_m - a.y_m * b.x_m,
    };
}


bool vf_fit_plane(
    const localization_point3d_t *points,
    size_t count,
    vf_plane_frame_t *frame
)
{
    if (points == NULL || frame == NULL || count < 3U) {
        return false;
    }

    localization_point3d_t centroid = { 0.0, 0.0, 0.0 };
    for (size_t i = 0U; i < count; ++i) {
        centroid.x_m += points[i].x_m;
        centroid.y_m += points[i].y_m;
        centroid.z_m += points[i].z_m;
    }
    centroid.x_m /= (double)count;
    centroid.y_m /= (double)count;
    centroid.z_m /= (double)count;

    double covariance[3][3] = { { 0.0 } };
    for (size_t i = 0U; i < count; ++i) {
        const double d[3] = {
            points[i].x_m - centroid.x_m,
            points[i].y_m - centroid.y_m,
            points[i].z_m - centroid.z_m,
        };
        for (size_t r = 0U; r < 3U; ++r) {
            for (size_t c = 0U; c < 3U; ++c) {
                covariance[r][c] += d[r] * d[c];
            }
        }
    }

    double vectors[3][3];
    jacobi_eigen_3x3(covariance, vectors);

    size_t order[3] = { 0U, 1U, 2U };
    for (size_t i = 0U; i < 3U; ++i) {
        for (size_t j = i + 1U; j < 3U; ++j) {
            if (covariance[order[j]][order[j]] > covariance[order[i]][order[i]]) {
                const size_t swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
        }
    }

    const double largest = covariance[order[0]][order[0]];
    const double middle = covariance[order[1]][order[1]];
    /* Collinear points span no plane. */
    if (!(largest > 0.0) || !(middle > 1.0e-9 * largest)) {
        return false;
    }

    localization_point3d_t axis_u = {
        vectors[0][order[0]], vectors[1][order[0]], vectors[2][order[0]],
    };
    localization_point3d_t normal = {
        vectors[0][order[2]], vectors[1][order[2]], vectors[2][order[2]],
    };
    if (normal.z_m < 0.0) {
        normal.x_m = -normal.x_m;
        normal.y_m = -normal.y_m;
        normal.z_m = -normal.z_m;
    }

    frame->centroid = centroid;
    frame->axis_u = axis_u;
    frame->normal = normal;
    frame->axis_v = cross(normal, axis_u);
    return true;
}


void vf_to_plane(
    const vf_plane_frame_t *frame,
    localization_point3d_t point,
    double *u,
    double *v,
    double *w
)
{
    const double dx = point.x_m - frame->centroid.x_m;
    const double dy = point.y_m - frame->centroid.y_m;
    const double dz = point.z_m - frame->centroid.z_m;
    *u = dx * frame->axis_u.x_m + dy * frame->axis_u.y_m + dz * frame->axis_u.z_m;
    *v = dx * frame->axis_v.x_m + dy * frame->axis_v.y_m + dz * frame->axis_v.z_m;
    *w = dx * frame->normal.x_m + dy * frame->normal.y_m + dz * frame->normal.z_m;
}


localization_point3d_t vf_from_plane(
    const vf_plane_frame_t *frame,
    double u,
    double v,
    double w
)
{
    return (localization_point3d_t) {
        frame->centroid.x_m + u * frame->axis_u.x_m + v * frame->axis_v.x_m + w * frame->normal.x_m,
        frame->centroid.y_m + u * frame->axis_u.y_m + v * frame->axis_v.y_m + w * frame->normal.y_m,
        frame->centroid.z_m + u * frame->axis_u.z_m + v * frame->axis_v.z_m + w * frame->normal.z_m,
    };
}


static void sort_doubles(double *values, size_t count)
{
    for (size_t i = 1U; i < count; ++i) {
        const double value = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = value;
    }
}


static double sorted_median(const double *sorted, size_t count)
{
    return (count % 2U) != 0U
        ? sorted[count / 2U]
        : 0.5 * (sorted[count / 2U - 1U] + sorted[count / 2U]);
}


/* sum_i tanh((x_i - mu) / (2 s)): decreasing in mu. */
static double logistic_location_score(const double *samples, size_t count, double mu, double s)
{
    double sum = 0.0;
    for (size_t i = 0U; i < count; ++i) {
        sum += tanh((samples[i] - mu) / (2.0 * s));
    }
    return sum;
}


/* sum_i z_i tanh(z_i / 2) - n with z_i = (x_i - mu) / s: decreasing in s. */
static double logistic_scale_score(const double *samples, size_t count, double mu, double s)
{
    double sum = 0.0;
    for (size_t i = 0U; i < count; ++i) {
        const double z = (samples[i] - mu) / s;
        sum += z * tanh(0.5 * z);
    }
    return sum - (double)count;
}


bool vf_fit_logistic(
    const double *samples,
    size_t count,
    double *mu,
    double *s
)
{
    if (samples == NULL || mu == NULL || s == NULL || count == 0U || count > VF_MAX_LOGISTIC_SAMPLES) {
        return false;
    }

    double sorted[VF_MAX_LOGISTIC_SAMPLES];
    for (size_t i = 0U; i < count; ++i) {
        if (!isfinite(samples[i])) {
            return false;
        }
        sorted[i] = samples[i];
    }
    sort_doubles(sorted, count);

    const double median = sorted_median(sorted, count);
    const double low = sorted[0];
    const double high = sorted[count - 1U];
    if (count == 1U || !(high > low)) {
        *mu = median;
        *s = 0.0;
        return true;
    }

    double deviations[VF_MAX_LOGISTIC_SAMPLES];
    for (size_t i = 0U; i < count; ++i) {
        deviations[i] = fabs(sorted[i] - median);
    }
    sort_doubles(deviations, count);
    /* Logistic MAD = s ln 3. */
    double scale = sorted_median(deviations, count) / log(3.0);
    if (!(scale > 0.0)) {
        scale = (high - low) / (double)count;
    }
    double location = median;

    for (int iteration = 0; iteration < 200; ++iteration) {
        const double previous_location = location;
        const double previous_scale = scale;

        /* Location: root of the (monotone) score inside the sample range. */
        double lo = low;
        double hi = high;
        for (int step = 0; step < 80; ++step) {
            const double mid = 0.5 * (lo + hi);
            if (logistic_location_score(samples, count, mid, scale) > 0.0) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        location = 0.5 * (lo + hi);

        /* Scale: bisection in log space on the monotone scale score. */
        double log_lo = log((high - low) * 1.0e-9);
        double log_hi = log((high - low) * 10.0);
        if (logistic_scale_score(samples, count, location, exp(log_lo)) <= 0.0) {
            /* Every sample (numerically) at the location. */
            scale = 0.0;
            break;
        }
        for (int step = 0; step < 80; ++step) {
            const double mid = 0.5 * (log_lo + log_hi);
            if (logistic_scale_score(samples, count, location, exp(mid)) > 0.0) {
                log_lo = mid;
            } else {
                log_hi = mid;
            }
        }
        scale = exp(0.5 * (log_lo + log_hi));

        if (
            fabs(location - previous_location) <= 1.0e-12 * (high - low) &&
            fabs(scale - previous_scale) <= 1.0e-10 * previous_scale
        ) {
            break;
        }
    }

    if (!isfinite(location) || !isfinite(scale)) {
        return false;
    }
    *mu = location;
    *s = scale;
    return true;
}


bool vf_fit_travel_times(
    const double *arrival_s,
    const double *path_m,
    const double *weight,
    size_t count,
    double sigma_t_s,
    double prior_slowness,
    double prior_sigma,
    vf_time_fit_t *fit
)
{
    if (
        arrival_s == NULL || path_m == NULL || fit == NULL || count == 0U ||
        !(sigma_t_s > 0.0) || !isfinite(prior_slowness) || !isfinite(prior_sigma)
    ) {
        return false;
    }

    double sum_w = 0.0;
    double sum_l = 0.0;
    double sum_t = 0.0;
    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const double w = weight != NULL ? weight[i] : 1.0;
        if (!(w > 0.0)) {
            continue;
        }
        if (!isfinite(arrival_s[i]) || !isfinite(path_m[i])) {
            return false;
        }
        sum_w += w;
        sum_l += w * path_m[i];
        sum_t += w * arrival_s[i];
        ++used;
    }
    if (used == 0U || !(sum_w > 0.0)) {
        return false;
    }

    const double mean_l = sum_l / sum_w;
    const double mean_t = sum_t / sum_w;
    double sxx = 0.0;
    double sxy = 0.0;
    for (size_t i = 0U; i < count; ++i) {
        const double w = weight != NULL ? weight[i] : 1.0;
        if (!(w > 0.0)) {
            continue;
        }
        sxx += w * (path_m[i] - mean_l) * (path_m[i] - mean_l);
        sxy += w * (path_m[i] - mean_l) * (arrival_s[i] - mean_t);
    }

    double slowness = 0.0;
    if (prior_sigma == 0.0) {
        slowness = prior_slowness;
    } else if (prior_sigma > 0.0) {
        /* Prior folded in on the same sigma_t-normalized scale. */
        const double lambda = (sigma_t_s * sigma_t_s) / (prior_sigma * prior_sigma);
        slowness = (sxy + lambda * prior_slowness) / (sxx + lambda);
    } else {
        /* Undetermined unless the path lengths actually differ. */
        if (!(sxx > 1.0e-18 * (sum_w * (mean_l * mean_l + 1.0)))) {
            return false;
        }
        slowness = sxy / sxx;
    }
    const double t0 = mean_t - slowness * mean_l;

    double chi_square = 0.0;
    double sum_sq = 0.0;
    for (size_t i = 0U; i < count; ++i) {
        const double w = weight != NULL ? weight[i] : 1.0;
        if (!(w > 0.0)) {
            continue;
        }
        const double residual = arrival_s[i] - t0 - slowness * path_m[i];
        chi_square += w * residual * residual / (sigma_t_s * sigma_t_s);
        sum_sq += residual * residual;
    }
    if (prior_sigma > 0.0) {
        const double prior_residual = (slowness - prior_slowness) / prior_sigma;
        chi_square += prior_residual * prior_residual;
    }

    fit->t0_s = t0;
    fit->slowness_s_per_m = slowness;
    fit->chi_square = chi_square;
    fit->rms_s = sqrt(sum_sq / (double)used);
    return isfinite(t0) && isfinite(slowness) && isfinite(chi_square);
}


bool vf_fit_travel_times_bounded(
    const double *arrival_s,
    const double *path_m,
    const double *weight,
    size_t count,
    double sigma_t_s,
    double fallback_velocity_mps,
    const localization_velocity_bounds_t *bounds,
    vf_time_fit_t *fit,
    bool *at_bound_out
)
{
    if (at_bound_out != NULL) {
        *at_bound_out = false;
    }
    if (fit == NULL || !(fallback_velocity_mps > 0.0) || !isfinite(fallback_velocity_mps)) {
        return false;
    }

    const bool bounded = bounds != NULL && bounds->set;
    if (bounded && localization_velocity_bounds_validate(bounds) != LOCALIZATION_VELOCITY_BOUNDS_VALID) {
        return false;
    }

    vf_time_fit_t free_fit;
    if (
        vf_fit_travel_times(arrival_s, path_m, weight, count, sigma_t_s, 0.0, -1.0, &free_fit) &&
        free_fit.slowness_s_per_m > 0.0
    ) {
        const double velocity = 1.0 / free_fit.slowness_s_per_m;
        if (!bounded) {
            *fit = free_fit;
            return true;
        }
        if (
            bounds->v_min_mps != bounds->v_max_mps &&
            velocity >= bounds->v_min_mps && velocity <= bounds->v_max_mps
        ) {
            *fit = free_fit;
            return true;
        }

        const double fixed_velocity = bounds->v_min_mps == bounds->v_max_mps
            ? bounds->v_min_mps
            : (velocity < bounds->v_min_mps ? bounds->v_min_mps : bounds->v_max_mps);
        const bool fitted = vf_fit_travel_times(
            arrival_s, path_m, weight, count, sigma_t_s, 1.0 / fixed_velocity, 0.0, fit
        );
        if (fitted && at_bound_out != NULL) {
            *at_bound_out = true;
        }
        return fitted;
    }

    const double fixed_velocity = bounded
        ? fmin(bounds->v_max_mps, fmax(bounds->v_min_mps, fallback_velocity_mps))
        : fallback_velocity_mps;
    const bool fitted = vf_fit_travel_times(
        arrival_s, path_m, weight, count, sigma_t_s, 1.0 / fixed_velocity, 0.0, fit
    );
    if (
        fitted && bounded && at_bound_out != NULL &&
        (bounds->v_min_mps == bounds->v_max_mps || fixed_velocity != fallback_velocity_mps)
    ) {
        *at_bound_out = true;
    }
    return fitted;
}
