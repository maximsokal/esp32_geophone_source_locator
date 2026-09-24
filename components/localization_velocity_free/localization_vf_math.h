#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "localization.h"

/*
 * Private numerical helpers shared by the velocity-free backends (TDCAS,
 * PSAFUVS, SUEA). Pure, allocation-free and bounded: every routine works on
 * caller stack arrays no larger than the constants below.
 */

#define VF_MAX_ROWS 32U
#define VF_MAX_COLS 6U


/*
 * Least squares min |A x - b| for a rows x cols row-major A (rows >= cols),
 * via column-equilibrated Householder QR. Returns false when A is
 * numerically rank deficient (a pivot below rank_tolerance x the largest,
 * after equilibration) or the result is non-finite.
 */
bool vf_least_squares(
    const double *a,
    const double *b,
    size_t rows,
    size_t cols,
    double rank_tolerance,
    double *x
);

/* Solves the square system A x = b (n <= VF_MAX_COLS) by Gaussian
 * elimination with partial pivoting. False for a (near-)singular A. */
bool vf_solve_square(
    const double *a,
    const double *b,
    size_t n,
    double *x
);


/* Least-squares plane through the points: centroid, unit normal oriented
 * with normal.z >= 0 (the side "above" the plane), and two in-plane unit
 * axes completing a right-handed frame (axis_u x axis_v = normal). */
typedef struct
{
    localization_point3d_t centroid;
    localization_point3d_t axis_u;
    localization_point3d_t axis_v;
    localization_point3d_t normal;

} vf_plane_frame_t;

bool vf_fit_plane(
    const localization_point3d_t *points,
    size_t count,
    vf_plane_frame_t *frame
);

/* Frame coordinates (u, v, w) of a point, and back. */
void vf_to_plane(
    const vf_plane_frame_t *frame,
    localization_point3d_t point,
    double *u,
    double *v,
    double *w
);

localization_point3d_t vf_from_plane(
    const vf_plane_frame_t *frame,
    double u,
    double v,
    double w
);


/*
 * Maximum-likelihood fit of a logistic distribution
 * f(x) = exp(-(x - mu)/s) / (s (1 + exp(-(x - mu)/s))^2)
 * to the samples. mu is the location (mean = median = mode), s > 0 the
 * scale (standard deviation = s pi / sqrt(3)). A single sample, or samples
 * that all coincide, return that value with s = 0.
 */
bool vf_fit_logistic(
    const double *samples,
    size_t count,
    double *mu,
    double *s
);


/*
 * Velocity-free travel-time fit: minimizes
 *   sum_i w_i (t_i - t0 - slowness * L_i)^2 / sigma_t^2
 *     [+ (slowness - prior_slowness)^2 / prior_sigma^2 when prior_sigma > 0]
 * over (t0, slowness). prior_sigma < 0 means no prior; prior_sigma == 0
 * fixes slowness = prior_slowness. Returns false when the fit is
 * undetermined (fewer than two distinct path lengths without a prior).
 * chi_square_out is the minimized objective, rms_out the unweighted RMS
 * of the time residuals (seconds).
 */
typedef struct
{
    double t0_s;
    double slowness_s_per_m;
    double chi_square;
    double rms_s;

} vf_time_fit_t;

bool vf_fit_travel_times(
    const double *arrival_s,
    const double *path_m,
    const double *weight,
    size_t count,
    double sigma_t_s,
    double prior_slowness,
    double prior_sigma,
    vf_time_fit_t *fit
);


double vf_distance(localization_point3d_t a, localization_point3d_t b);
