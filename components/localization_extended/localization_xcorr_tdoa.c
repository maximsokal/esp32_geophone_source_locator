#include "localization_xcorr_tdoa.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_ext_fit.h"


#define XCORR_DEFAULT_MIN_CORRELATION 0.7
#define XCORR_CORRELATION_CAP 0.995
#define XCORR_DEFAULT_LATTICE_POINTS 8000U
#define XCORR_IRLS_ROUNDS 20U
#define XCORR_OUTLIER_SIGMA 3.0
#define XCORR_INTERP_HALF 8


static double min_correlation(const localization_xcorr_tdoa_config_t *config)
{
    return config->min_correlation > 0.0 ? config->min_correlation : XCORR_DEFAULT_MIN_CORRELATION;
}


static double correlation_gain(double c)
{
    if (c > XCORR_CORRELATION_CAP) {
        c = XCORR_CORRELATION_CAP;
    }
    return c * c / (1.0 - c * c);
}


/* Correlation of a = x[start, start + w) with b = y[start + lag, ...):
 * the raw product sum (for locating the peak) and the normalized
 * coefficient (for judging it); false when b leaves the record.
 *
 * The peak is located on the raw sum: normalizing each lag by its own
 * window energy moves the peak towards lags where the sliding window
 * clips the wavelet -- a bias of ~0.1-0.3 sample even without noise. */
static bool correlation_at(
    const float *x,
    const float *y,
    size_t count,
    long start,
    size_t window,
    long lag,
    double *raw,
    double *normalized
)
{
    const long b_start = start + lag;
    if (start < 0 || b_start < 0 || (size_t)start + window > count || (size_t)b_start + window > count) {
        return false;
    }
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t n = 0U; n < window; ++n) {
        const double a = x[(size_t)start + n];
        const double b = y[(size_t)b_start + n];
        ab += a * b;
        aa += a * a;
        bb += b * b;
    }
    if (!(aa > 0.0) || !(bb > 0.0)) {
        return false;
    }
    *raw = ab;
    *normalized = ab / sqrt(aa * bb);
    return true;
}


/* Lanczos (a = XCORR_INTERP_HALF) interpolation of y at fractional sample
 * position `position`; NAN when the kernel leaves the record. */
static double lanczos_sample(const float *y, size_t count, double position)
{
    const double pi = 3.141592653589793;
    const double a = (double)XCORR_INTERP_HALF;
    const long base = (long)floor(position);
    if (base - XCORR_INTERP_HALF + 1 < 0 || (size_t)(base + XCORR_INTERP_HALF) >= count) {
        return NAN;
    }
    double sum = 0.0;
    for (long m = base - XCORR_INTERP_HALF + 1; m <= base + XCORR_INTERP_HALF; ++m) {
        const double t = position - (double)m;
        const double kernel = fabs(t) < 1.0e-12
            ? 1.0
            : a * sin(pi * t) * sin(pi * t / a) / (pi * pi * t * t);
        sum += kernel * (double)y[m];
    }
    return sum;
}


/* Raw correlation at a fractional lag: the second record is resampled
 * band-limited (it is sampled far above its wavelet's band). */
static double correlation_fractional(
    const float *x,
    const float *y,
    size_t count,
    long start,
    size_t window,
    double lag
)
{
    double sum = 0.0;
    for (size_t n = 0U; n < window; ++n) {
        const double b = lanczos_sample(y, count, (double)start + (double)n + lag);
        if (!isfinite(b)) {
            return NAN;
        }
        sum += (double)x[(size_t)start + n] * b;
    }
    return sum;
}


/* Delay of `second` behind `first` (in samples), windowed on `first`
 * around `onset`. */
static bool measure_pair(
    const localization_xcorr_channel_t *first,
    const localization_xcorr_channel_t *second,
    double onset,
    long lag_low,
    long lag_high,
    size_t window,
    double *delay_samples,
    double *correlation
)
{
    const long start = (long)floor(onset) - (long)(window / 4U);
    double best = -INFINITY;
    double best_coefficient = 0.0;
    long best_lag = 0;
    bool any = false;
    for (long lag = lag_low; lag <= lag_high; ++lag) {
        double raw = 0.0;
        double coefficient = 0.0;
        if (!correlation_at(first->samples, second->samples, first->sample_count, start, window, lag, &raw, &coefficient)) {
            continue;
        }
        if (raw > best) {
            best = raw;
            best_coefficient = coefficient;
            best_lag = lag;
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    /* Sub-sample peak: the correlation is evaluated at fractional lags by
     * band-limited (Lanczos) resampling of the second record and maximized
     * by golden section. Interpolating the three correlation samples
     * around the peak instead (parabola, cosine) is biased by ~0.1 sample
     * on a geophone wavelet at 500 Hz -- ~0.06 m at 300 m/s. */
    double low = (double)best_lag - 0.6;
    double high = (double)best_lag + 0.6;
    const double golden = 0.6180339887498949;
    double x1 = high - golden * (high - low);
    double x2 = low + golden * (high - low);
    const long start_index = start;
    double f1 = correlation_fractional(first->samples, second->samples, first->sample_count, start_index, window, x1);
    double f2 = correlation_fractional(first->samples, second->samples, first->sample_count, start_index, window, x2);
    bool interpolated = isfinite(f1) && isfinite(f2);
    for (int iteration = 0; interpolated && iteration < 32; ++iteration) {
        if (f1 < f2) {
            low = x1;
            x1 = x2;
            f1 = f2;
            x2 = low + golden * (high - low);
            f2 = correlation_fractional(first->samples, second->samples, first->sample_count, start_index, window, x2);
        } else {
            high = x2;
            x2 = x1;
            f2 = f1;
            x1 = high - golden * (high - low);
            f1 = correlation_fractional(first->samples, second->samples, first->sample_count, start_index, window, x1);
        }
        interpolated = isfinite(f1) && isfinite(f2);
    }
    const double offset = interpolated ? 0.5 * (low + high) - (double)best_lag : 0.0;
    *delay_samples = (double)best_lag + offset;
    *correlation = best_coefficient;
    return true;
}


static bool channels_valid(const localization_xcorr_channel_t *channels, size_t count)
{
    if (channels == NULL || count < 2U || count > LOCALIZATION_EXTENDED_MAX_SENSORS) {
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        const localization_xcorr_channel_t *c = &channels[i];
        if (
            c->samples == NULL || c->sample_count == 0U || c->sample_rate_hz == 0U ||
            c->sample_count != channels[0].sample_count || c->sample_rate_hz != channels[0].sample_rate_hz ||
            !isfinite(c->sensor_position.x_m) || !isfinite(c->sensor_position.y_m) ||
            !isfinite(c->sensor_position.z_m) || (c->pick_valid && !(c->pick_sample >= 0.0))
        ) {
            return false;
        }
    }
    return true;
}


static bool config_valid(const localization_xcorr_tdoa_config_t *config)
{
    return
        config != NULL &&
        config->propagation_speed_mps > 0.0 && isfinite(config->propagation_speed_mps) &&
        config->timing_uncertainty_us > 0.0 && isfinite(config->timing_uncertainty_us) &&
        config->window_samples >= 4U && config->max_lag_samples > 0U &&
        min_correlation(config) < 1.0;
}


esp_err_t localization_xcorr_measure_pairs(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_pair_t *pairs_out,
    size_t *pair_count_out,
    size_t *pairs_total_out
)
{
    if (
        !channels_valid(channels, channel_count) || !config_valid(config) ||
        pairs_out == NULL || pair_count_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *pair_count_out = 0U;
    const size_t total = channel_count * (channel_count - 1U) / 2U;
    if (pairs_total_out != NULL) {
        *pairs_total_out = total;
    }

    bool anchored = false;
    for (size_t i = 0U; i < channel_count; ++i) {
        anchored = anchored || channels[i].pick_valid;
    }
    if (!anchored) {
        return ESP_ERR_NOT_FOUND;
    }

    const double sample_period_s = 1.0 / (double)channels[0].sample_rate_hz;
    const double c_min = min_correlation(config);
    const double gain_min = correlation_gain(c_min);
    const long max_lag = (long)config->max_lag_samples;

    for (size_t i = 0U; i < channel_count; ++i) {
        for (size_t j = i + 1U; j < channel_count; ++j) {
            /* Window on a channel that has an onset; a pair with none is
             * not anchored anywhere and is skipped. */
            const bool swap = !channels[i].pick_valid;
            if (swap && !channels[j].pick_valid) {
                continue;
            }
            const localization_xcorr_channel_t *first = swap ? &channels[j] : &channels[i];
            const localization_xcorr_channel_t *second = swap ? &channels[i] : &channels[j];
            long low = -max_lag;
            long high = max_lag;
            if (config->max_pick_disagreement_samples > 0.0 && first->pick_valid && second->pick_valid) {
                const double centre = second->pick_sample - first->pick_sample;
                const long gate_low = (long)floor(centre - config->max_pick_disagreement_samples);
                const long gate_high = (long)ceil(centre + config->max_pick_disagreement_samples);
                low = gate_low > low ? gate_low : low;
                high = gate_high < high ? gate_high : high;
            }
            if (low > high) {
                continue;
            }
            double delay = 0.0;
            double correlation = 0.0;
            if (
                !measure_pair(first, second, first->pick_sample, low, high, config->window_samples,
                              &delay, &correlation) ||
                correlation < c_min
            ) {
                continue;
            }
            localization_xcorr_pair_t *pair = &pairs_out[(*pair_count_out)++];
            pair->first = i;
            pair->second = j;
            pair->delay_s = (swap ? -delay : delay) * sample_period_s;
            pair->correlation = correlation;
            pair->sigma_s = config->timing_uncertainty_us * 1.0e-6 * sqrt(gain_min / correlation_gain(correlation));
        }
    }
    return ESP_OK;
}


typedef struct
{
    const localization_xcorr_channel_t *channels;
    const localization_xcorr_pair_t *pairs;
    size_t pair_count;
    double weight[LOCALIZATION_XCORR_MAX_PAIRS];
    double slowness;
    bool planar;
    const localization_extended_bounds_t *bounds;

} pair_problem_t;


static localization_point3d_t pair_position(const pair_problem_t *problem, const double *params)
{
    localization_point3d_t point = { params[0], params[1], problem->bounds->min_corner.z_m };
    if (!problem->planar) {
        point.z_m = params[2];
    }
    return point;
}


static double pair_residual_s(const pair_problem_t *problem, size_t k, localization_point3d_t point)
{
    const localization_xcorr_pair_t *pair = &problem->pairs[k];
    return pair->delay_s - problem->slowness * (
        ext_distance(point, problem->channels[pair->second].sensor_position) -
        ext_distance(point, problem->channels[pair->first].sensor_position)
    );
}


static bool pair_row(size_t row, const double *params, double *residual, double *gradient, void *context)
{
    const pair_problem_t *problem = context;
    if (!(problem->weight[row] > 0.0)) {
        return false;
    }
    const localization_xcorr_pair_t *pair = &problem->pairs[row];
    const localization_point3d_t point = pair_position(problem, params);
    const localization_point3d_t a = problem->channels[pair->first].sensor_position;
    const localization_point3d_t b = problem->channels[pair->second].sensor_position;
    double da = ext_distance(point, a);
    double db = ext_distance(point, b);
    da = da < 1.0e-9 ? 1.0e-9 : da;
    db = db < 1.0e-9 ? 1.0e-9 : db;
    const double scale = sqrt(problem->weight[row]) / pair->sigma_s;
    *residual = scale * (pair->delay_s - problem->slowness * (db - da));
    const double f = -scale * problem->slowness;
    gradient[0] = f * ((point.x_m - b.x_m) / db - (point.x_m - a.x_m) / da);
    gradient[1] = f * ((point.y_m - b.y_m) / db - (point.y_m - a.y_m) / da);
    if (!problem->planar) {
        gradient[2] = f * ((point.z_m - b.z_m) / db - (point.z_m - a.z_m) / da);
    }
    return true;
}


static void pair_project(double *params, void *context)
{
    const pair_problem_t *problem = context;
    const localization_point3d_t clamped = ext_clamp_to_bounds(problem->bounds, pair_position(problem, params));
    params[0] = clamped.x_m;
    params[1] = clamped.y_m;
    if (!problem->planar) {
        params[2] = clamped.z_m;
    }
}


/* Least-squares network adjustment: per-channel times tau with
 * d_ij = tau_j - tau_i over the used pairs, by Gauss-Seidel sweeps (each
 * tau becomes the weighted mean its pairs imply) -- no matrix, so no stack
 * for one. The first used channel is held at 0. */
static void network_adjust(
    const localization_xcorr_pair_t *pairs,
    size_t pair_count,
    const double *weight,
    size_t channel_count,
    uint32_t used_channels,
    localization_xcorr_tdoa_result_t *result
)
{
    double tau[LOCALIZATION_EXTENDED_MAX_SENSORS] = { 0 };
    size_t reference = LOCALIZATION_EXTENDED_MAX_SENSORS;
    for (size_t c = 0U; c < channel_count && reference == LOCALIZATION_EXTENDED_MAX_SENSORS; ++c) {
        if ((used_channels & (1U << c)) != 0U) {
            reference = c;
        }
    }
    if (reference == LOCALIZATION_EXTENDED_MAX_SENSORS) {
        return;
    }
    for (int sweep = 0; sweep < 200; ++sweep) {
        double change = 0.0;
        for (size_t c = 0U; c < channel_count; ++c) {
            if (c == reference || (used_channels & (1U << c)) == 0U) {
                continue;
            }
            double sum = 0.0;
            double norm = 0.0;
            for (size_t k = 0U; k < pair_count; ++k) {
                if (!(weight[k] > 0.0)) {
                    continue;
                }
                const double w = weight[k] / (pairs[k].sigma_s * pairs[k].sigma_s);
                if (pairs[k].second == c) {
                    sum += w * (tau[pairs[k].first] + pairs[k].delay_s);
                    norm += w;
                } else if (pairs[k].first == c) {
                    sum += w * (tau[pairs[k].second] - pairs[k].delay_s);
                    norm += w;
                }
            }
            if (norm > 0.0) {
                const double updated = sum / norm;
                change = fmax(change, fabs(updated - tau[c]));
                tau[c] = updated;
            }
        }
        if (change < 1.0e-12) {
            break;
        }
    }
    double earliest = 0.0;
    for (size_t c = 0U; c < channel_count; ++c) {
        if ((used_channels & (1U << c)) != 0U) {
            earliest = fmin(earliest, tau[c]);
        }
    }
    double sum = 0.0;
    size_t used = 0U;
    for (size_t k = 0U; k < pair_count; ++k) {
        if (weight[k] > 0.0) {
            const double r = pairs[k].delay_s - (tau[pairs[k].second] - tau[pairs[k].first]);
            sum += r * r;
            ++used;
        }
    }
    result->closure_rms_us = used > 0U ? sqrt(sum / (double)used) * 1.0e6 : 0.0;
    for (size_t c = 0U; c < channel_count; ++c) {
        if ((used_channels & (1U << c)) != 0U) {
            result->relative_delay_us[c] = (tau[c] - earliest) * 1.0e6;
        }
    }
}


/* The pair weights live on the heap (pair_problem_t): they would
 * otherwise sit on the 8 KiB HTTPS task's stack under
 * Levenberg-Marquardt's frame. */
static esp_err_t locate_with(
    pair_problem_t *problem,
    double *final_weight,
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_xcorr_pair_t *pairs,
    size_t pair_count,
    const localization_extended_bounds_t *bounds,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_tdoa_result_t *result
)
{
    if (
        channels == NULL || channel_count < 2U || channel_count > LOCALIZATION_EXTENDED_MAX_SENSORS ||
        (pairs == NULL && pair_count > 0U) || pair_count > LOCALIZATION_XCORR_MAX_PAIRS ||
        !localization_extended_bounds_valid(bounds) || !config_valid(config) || result == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t k = 0U; k < pair_count; ++k) {
        if (
            pairs[k].first >= channel_count || pairs[k].second >= channel_count ||
            pairs[k].first == pairs[k].second || !isfinite(pairs[k].delay_s) ||
            !(pairs[k].sigma_s > 0.0) || !isfinite(pairs[k].sigma_s)
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memset(result, 0, sizeof(*result));
    result->pairs_total = channel_count * (channel_count - 1U) / 2U;
    result->planar = localization_extended_bounds_planar(bounds);
    result->pairs_measured = pair_count;

    const size_t spatial = result->planar ? 2U : 3U;
    uint32_t channels_in_pairs = 0U;
    double correlation_sum = 0.0;
    for (size_t k = 0U; k < pair_count; ++k) {
        channels_in_pairs |= (1U << pairs[k].first) | (1U << pairs[k].second);
        correlation_sum += pairs[k].correlation;
    }
    result->mean_correlation = pair_count > 0U ? correlation_sum / (double)pair_count : 0.0;
    size_t channels_present = 0U;
    for (uint32_t bits = channels_in_pairs; bits != 0U; bits &= bits - 1U) {
        ++channels_present;
    }
    if (pair_count < spatial || channels_present < spatial + 1U) {
        result->status = LOCALIZATION_XCORR_TDOA_INSUFFICIENT_PAIRS;
        result->degrees_of_freedom = (int32_t)channels_present - 1 - (int32_t)spatial;
        return ESP_OK;
    }

    memset(problem, 0, sizeof(*problem));
    problem->channels = channels;
    problem->pairs = pairs;
    problem->pair_count = pair_count;
    problem->slowness = 1.0 / config->propagation_speed_mps;
    problem->planar = result->planar;
    problem->bounds = bounds;
    for (size_t k = 0U; k < pair_count; ++k) {
        problem->weight[k] = 1.0;
    }

    /* Seed: the admitted lattice point with the least weighted pair misfit. */
    ext_lattice_t lattice;
    if (!ext_lattice_init(
            bounds, config->max_lattice_points > 0U ? config->max_lattice_points : XCORR_DEFAULT_LATTICE_POINTS,
            &lattice
        )) {
        return ESP_ERR_INVALID_ARG;
    }
    double best_cost = INFINITY;
    localization_point3d_t position = lattice.origin;
    const size_t lattice_count = ext_lattice_count(&lattice);
    for (size_t index = 0U; index < lattice_count; ++index) {
        const localization_point3d_t point = ext_lattice_point(&lattice, index);
        if (!localization_candidate_admitted(&config->candidate_filter, &point)) {
            continue;
        }
        double cost = 0.0;
        for (size_t k = 0U; k < pair_count; ++k) {
            const double u = pair_residual_s(problem, k, point) / pairs[k].sigma_s;
            /* Seed with the robust loss too: one cycle-skipped pair must
             * not pick the basin. */
            cost += localization_extended_robust_rho(LOCALIZATION_EXTENDED_LOSS_HUBER, u, LOCALIZATION_EXTENDED_HUBER_K);
        }
        if (cost < best_cost) {
            best_cost = cost;
            position = point;
        }
    }
    if (!isfinite(best_cost)) {
        result->status = LOCALIZATION_XCORR_TDOA_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }

    const localization_extended_loss_t loss = config->loss;
    const double k_tuning = config->tuning_k > 0.0
        ? config->tuning_k
        : (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY ? LOCALIZATION_EXTENDED_TUKEY_K : LOCALIZATION_EXTENDED_HUBER_K);
    const double scale[3] = { 1.0, 1.0, 1.0 };
    double params[3] = { position.x_m, position.y_m, position.z_m };
    for (size_t round = 0U; round < XCORR_IRLS_ROUNDS; ++round) {
        double change = 0.0;
        const localization_point3d_t current = pair_position(problem, params);
        for (size_t k = 0U; k < pair_count; ++k) {
            const double u = pair_residual_s(problem, k, current) / pairs[k].sigma_s;
            double w = localization_extended_robust_weight(loss, u, k_tuning);
            w = w < 1.0e-6 ? 1.0e-6 : w;
            change = fmax(change, fabs(w - problem->weight[k]));
            problem->weight[k] = w;
        }
        double trial[3] = { params[0], params[1], params[2] };
        ext_lm_result_t lm;
        if (ext_lm(spatial, pair_count, pair_row, pair_project, problem, scale, 1.0e-7, 60U, trial, &lm)) {
            const localization_point3d_t refined = pair_position(problem, trial);
            if (localization_candidate_admitted(&config->candidate_filter, &refined)) {
                memcpy(params, trial, sizeof(params));
            }
        }
        if (round > 0U && change < 1.0e-4) {
            break;
        }
    }
    position = pair_position(problem, params);

    /* Pairs the fit cannot explain are reported and left out of the
     * closure / statistics. */
    double sum = 0.0;
    uint32_t used_channels = 0U;
    for (size_t k = 0U; k < pair_count; ++k) {
        const double r = pair_residual_s(problem, k, position);
        if (fabs(r / pairs[k].sigma_s) > XCORR_OUTLIER_SIGMA) {
            result->pair_outlier_mask |= 1U << k;
            final_weight[k] = 0.0;
        } else {
            final_weight[k] = 1.0;
            sum += r * r;
            ++result->pairs_used;
            used_channels |= (1U << pairs[k].first) | (1U << pairs[k].second);
        }
    }
    result->channel_mask_used = used_channels;
    for (uint32_t bits = used_channels; bits != 0U; bits &= bits - 1U) {
        ++result->channels_used;
    }
    result->rms_pair_residual_us = result->pairs_used > 0U ? sqrt(sum / (double)result->pairs_used) * 1.0e6 : 0.0;
    network_adjust(pairs, pair_count, final_weight, channel_count, used_channels, result);

    result->position = position;
    result->degrees_of_freedom = (int32_t)result->channels_used - 1 - (int32_t)spatial;
    for (size_t k = 0U; k < pair_count; ++k) {
        problem->weight[k] = final_weight[k];
    }
    result->normal_rcond = ext_normal_rcond(spatial, pair_count, pair_row, problem, params);
    result->status = result->channels_used < spatial + 1U
        ? LOCALIZATION_XCORR_TDOA_INSUFFICIENT_PAIRS
        : (result->normal_rcond < EXT_UNDERDETERMINED_RCOND
            ? LOCALIZATION_XCORR_TDOA_UNDERDETERMINED
            : LOCALIZATION_XCORR_TDOA_SOLVED);
    return ESP_OK;
}


esp_err_t localization_xcorr_tdoa_locate(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_xcorr_pair_t *pairs,
    size_t pair_count,
    const localization_extended_bounds_t *bounds,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_tdoa_result_t *result
)
{
    pair_problem_t *problem = malloc(sizeof(*problem));
    double *final_weight = malloc(LOCALIZATION_XCORR_MAX_PAIRS * sizeof(double));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (problem != NULL && final_weight != NULL) {
        err = locate_with(problem, final_weight, channels, channel_count, pairs, pair_count, bounds, config, result);
    }
    free(problem);
    free(final_weight);
    return err;
}


esp_err_t localization_xcorr_tdoa_solve(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_extended_bounds_t *bounds,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_tdoa_result_t *result
)
{
    if (result == NULL || !localization_extended_bounds_valid(bounds)) {
        return ESP_ERR_INVALID_ARG;
    }
    localization_xcorr_pair_t *pairs = malloc(LOCALIZATION_XCORR_MAX_PAIRS * sizeof(*pairs));
    if (pairs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t pair_count = 0U;
    size_t total = 0U;
    esp_err_t err = localization_xcorr_measure_pairs(channels, channel_count, config, pairs, &pair_count, &total);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(result, 0, sizeof(*result));
        result->status = LOCALIZATION_XCORR_TDOA_NO_ANCHOR;
        result->planar = localization_extended_bounds_planar(bounds);
        result->pairs_total = total;
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = localization_xcorr_tdoa_locate(channels, channel_count, pairs, pair_count, bounds, config, result);
    }
    free(pairs);
    return err;
}
