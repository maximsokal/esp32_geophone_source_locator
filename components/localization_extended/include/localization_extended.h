#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization.h"

/*
 * localization_extended: source-location methods beyond the pick-based
 * straight-ray fits, each in its own header:
 *
 *   localization_xcorr_tdoa.h  all-pairs waveform cross-correlation TDOA
 *                              with network closure and robust pair weights
 *   localization_robust.h      M-estimator (Huber / Tukey) with an
 *                              exhaustive minimal-subset consensus (MSAC)
 *   localization_psp.h         joint P-TDOA + P-S fit ("PSP") with
 *                              per-sensor S confidence
 *   localization_eikonal.h     fast-marching eikonal travel times in a
 *                              heterogeneous velocity grid, source location
 *                              in it, and travel-time tomography from
 *                              calibration hits
 *   localization_bayes.h       grid posterior with velocity and origin time
 *                              marginalized: MAP, posterior mean, standard
 *                              deviations and a credible region
 *
 * This header holds what they share.
 */

#define LOCALIZATION_EXTENDED_MAX_SENSORS LOCALIZATION_MAX_SENSORS


/*
 * A search box: min/max corners and the lattice step. min_corner.z_m ==
 * max_corner.z_m selects a planar (single layer) search, the same
 * convention as the grid-search backends.
 */
typedef struct
{
    localization_point3d_t min_corner;
    localization_point3d_t max_corner;
    double step_m;

} localization_extended_bounds_t;


typedef enum
{
    LOCALIZATION_EXTENDED_LOSS_HUBER = 0,
    LOCALIZATION_EXTENDED_LOSS_TUKEY

} localization_extended_loss_t;

/* The usual 95%-efficiency tuning constants, in units of the residual's
 * standard deviation. */
#define LOCALIZATION_EXTENDED_HUBER_K 1.345
#define LOCALIZATION_EXTENDED_TUKEY_K 4.685


bool localization_extended_bounds_valid(const localization_extended_bounds_t *bounds);

static inline bool localization_extended_bounds_planar(const localization_extended_bounds_t *bounds)
{
    return bounds->min_corner.z_m == bounds->max_corner.z_m;
}

/* IRLS weight psi(u)/u and loss rho(u) of a standardized residual u. */
double localization_extended_robust_weight(localization_extended_loss_t loss, double u, double k);
double localization_extended_robust_rho(localization_extended_loss_t loss, double u, double k);
