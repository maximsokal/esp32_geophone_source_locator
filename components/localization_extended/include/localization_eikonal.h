#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_extended.h"

/*
 * Eikonal travel times in a heterogeneous velocity model, source location
 * in it, and travel-time tomography from calibration hits.
 *
 * The first-arrival time T(x) from a point obeys the eikonal equation
 * |grad T| = 1 / v(x). It is solved on a node lattice by the fast marching
 * method (Sethian), with the second-order upwind stencil where two known
 * nodes are available and exact straight-ray times in a small ball around
 * the source (where the first-order scheme is least accurate). Nodes with
 * no velocity (a scan-verified void) are impassable, so waves go around
 * them -- the physical version of "straight ray unless blocked".
 *
 * By reciprocity one field per sensor serves every candidate source:
 * T_i(x) = time from x to sensor i = time from sensor i to x.
 *
 * Location minimizes, over the lattice and then a finer local lattice,
 *   sum_i rho((t_i - t0 - a T_i(x)) / sigma)
 * with t0 profiled out and a a global slowness factor (1 = the model as
 * given) that is fixed, free, or held by a prior. With two picks to spare,
 * an implausible misfit (chi^2 beyond its mean + 3 sd) is tested against
 * every leave-one-out subset, and one pick is rejected when that alone
 * makes the fit plausible.
 *
 * Calibration (tomography): hits at known positions (and optionally known
 * times) give measured-minus-modelled travel times; each is spread back
 * along its ray (traced down the gradient of the sensor's field) as a
 * slowness correction (SIRT), smoothed, clamped to physical bounds and
 * kept only while the misfit falls. The result is a velocity map, not a
 * binary empty/filled map: slow (voided, loosened) zones come out slow.
 */

#define LOCALIZATION_EIKONAL_DEFAULT_MAX_NODES 40000U


typedef struct
{
    localization_point3d_t origin;   /* node (0, 0, 0) */
    double cell_m;
    bool resolution_limited;
    size_t nx;
    size_t ny;
    size_t nz;
    float *slowness_s_per_m;         /* per node; INFINITY = impassable */

} localization_eikonal_model_t;


/*
 * Node lattice from min_corner covering max_corner with spacing cell_m,
 * enlarged by the smallest integer factor that keeps the node count within
 * max_nodes (0: LOCALIZATION_EIKONAL_DEFAULT_MAX_NODES). min z == max z
 * gives a single layer (a 2D model). Every node starts at
 * background_velocity_mps.
 */
esp_err_t localization_eikonal_model_init(
    localization_eikonal_model_t *model,
    const localization_point3d_t *min_corner,
    const localization_point3d_t *max_corner,
    double cell_m,
    size_t max_nodes,
    double background_velocity_mps
);

void localization_eikonal_model_free(localization_eikonal_model_t *model);

size_t localization_eikonal_node_count(const localization_eikonal_model_t *model);

localization_point3d_t localization_eikonal_node_position(
    const localization_eikonal_model_t *model,
    size_t index
);

/* velocity <= 0 (or non-finite) makes the node impassable. */
void localization_eikonal_set_velocity(localization_eikonal_model_t *model, size_t index, double velocity_mps);

/* 0 for an impassable node. */
double localization_eikonal_velocity(const localization_eikonal_model_t *model, size_t index);

/* Velocity at a point for every node, e.g. from a scan: return <= 0 for
 * void. */
typedef double (*localization_eikonal_velocity_fn)(const localization_point3d_t *point, void *context);

void localization_eikonal_model_assign(
    localization_eikonal_model_t *model,
    localization_eikonal_velocity_fn velocity,
    void *context
);

size_t localization_eikonal_passable_count(const localization_eikonal_model_t *model);


/*
 * First-arrival times from `source` to every node into times[node_count]
 * (INFINITY where unreachable). ESP_ERR_NOT_FOUND when the source lies
 * outside the lattice or no passable node is in sight within five cells
 * of it;
 * ESP_ERR_NO_MEM when the marching workspace cannot be allocated.
 */
esp_err_t localization_eikonal_travel_times(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *source,
    float *times
);

/* Time at an arbitrary point: trilinear inside passable cells, otherwise
 * the best passable corner plus the straight hop to it; INFINITY when no
 * corner is reachable or the point is outside the lattice. */
double localization_eikonal_time_at(
    const localization_eikonal_model_t *model,
    const float *times,
    const localization_point3d_t *point
);


/* One field per sensor, allocated as a single block: fields[i] points at
 * sensor i's times. Unreachable sensors (ESP_ERR_NOT_FOUND from
 * travel_times) get a field of INFINITY and do not fail the call. */
typedef struct
{
    size_t count;
    size_t node_count;
    float *block;
    float *fields[LOCALIZATION_EXTENDED_MAX_SENSORS];

} localization_eikonal_fields_t;

esp_err_t localization_eikonal_fields_compute(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *sensors,
    size_t sensor_count,
    localization_eikonal_fields_t *fields
);

void localization_eikonal_fields_free(localization_eikonal_fields_t *fields);


/* A localization_travel_time_model_t over computed fields (sensor index =
 * field index), e.g. for semblance or the Bayesian posterior. */
typedef struct
{
    const localization_eikonal_model_t *model;
    const localization_eikonal_fields_t *fields;

} localization_eikonal_travel_time_context_t;

bool localization_eikonal_travel_time(
    size_t sensor_index,
    const localization_point3d_t *candidate,
    double *travel_time_s,
    void *context
);


/* ---- Source location ------------------------------------------------ */

typedef struct
{
    int64_t arrival_time_us;
    double weight;                 /* 0 = not used */

} localization_eikonal_pick_t;

typedef struct
{
    double timing_uncertainty_us;

    /* Global slowness factor a: sigma < 0 free, 0 fixed at 1 (the model
     * exactly), > 0 a Gaussian prior around 1 with this relative sigma. */
    double velocity_scale_sigma;

    localization_extended_loss_t loss;  /* default (0): Huber */
    double tuning_k;                    /* <= 0: the loss's default */
    uint32_t refine_divisor;            /* >= 2 */

    localization_candidate_filter_t candidate_filter;

} localization_eikonal_locate_config_t;

typedef enum
{
    LOCALIZATION_EIKONAL_SOLVED = 0,
    LOCALIZATION_EIKONAL_INSUFFICIENT_PICKS,
    LOCALIZATION_EIKONAL_NO_REACHABLE_CANDIDATE,
    LOCALIZATION_EIKONAL_NO_ALLOWED_CANDIDATE

} localization_eikonal_status_t;

typedef struct
{
    localization_eikonal_status_t status;

    localization_point3d_t position;
    int64_t emission_time_us;
    double velocity_scale;              /* fitted slowness factor a */
    double rms_residual_us;

    size_t picks_used;
    uint32_t pick_mask_used;
    uint32_t pick_mask_unreachable;     /* field INFINITY everywhere admitted */
    uint32_t pick_mask_rejected;        /* dropped by the leave-one-out test */
    uint32_t pick_mask_downweighted;    /* robust weight < 0.5 at the solution */
    size_t candidates_evaluated;
    double cell_m;
    bool resolution_limited;

} localization_eikonal_result_t;

/*
 * picks[i] belongs to fields->fields[i]; pick_count == fields->count.
 */
esp_err_t localization_eikonal_locate(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const localization_eikonal_pick_t *picks,
    size_t pick_count,
    const localization_eikonal_locate_config_t *config,
    localization_eikonal_result_t *result
);


/* ---- Calibration (travel-time tomography) --------------------------- */

typedef struct
{
    localization_point3d_t position;
    bool origin_time_known;
    int64_t origin_time_us;
    int64_t arrival_time_us[LOCALIZATION_EXTENDED_MAX_SENSORS];
    bool arrival_valid[LOCALIZATION_EXTENDED_MAX_SENSORS];

} localization_eikonal_shot_t;

typedef struct
{
    double timing_uncertainty_us;
    double min_velocity_mps;
    double max_velocity_mps;
    size_t max_iterations;
    double relaxation;                  /* (0, 1] */
    size_t smoothing_passes;

} localization_eikonal_calibration_config_t;

/* 10 iterations, relaxation 0.7, one smoothing pass; the caller sets the
 * timing uncertainty and velocity bounds. */
localization_eikonal_calibration_config_t localization_eikonal_calibration_default_config(void);

typedef struct
{
    size_t iterations;                  /* accepted updates */
    size_t rays_used;
    double rms_before_us;
    double rms_after_us;
    size_t nodes_updated;               /* nodes some ray crossed */
    double min_velocity_mps;            /* over updated nodes */
    double max_velocity_mps;

} localization_eikonal_calibration_result_t;

/*
 * Updates model->slowness_s_per_m in place from the shots (arrival_time_us
 * index = sensor index). Impassable nodes stay impassable. Needs at least
 * one shot with two valid arrivals (one with a known origin time).
 */
esp_err_t localization_eikonal_calibrate(
    localization_eikonal_model_t *model,
    const localization_point3d_t *sensors,
    size_t sensor_count,
    const localization_eikonal_shot_t *shots,
    size_t shot_count,
    const localization_eikonal_calibration_config_t *config,
    localization_eikonal_calibration_result_t *result
);
