#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define LOCALIZATION_MIN_SENSORS_3D  4
#define LOCALIZATION_MAX_SENSORS     8


typedef struct
{
    double x_m;
    double y_m;
    double z_m;

} localization_point3d_t;


/*
 * MAX-147: optional per-candidate admission test for the grid-search
 * backends (e.g. "not in scan-verified FREE space"). A zero-initialized
 * filter (allowed == NULL) admits every candidate.
 */
typedef bool (*localization_candidate_allowed_fn)(
    const localization_point3d_t *candidate,
    void *context
);

typedef struct
{
    localization_candidate_allowed_fn allowed;
    void *context;

} localization_candidate_filter_t;


static inline bool localization_candidate_admitted(
    const localization_candidate_filter_t *filter,
    const localization_point3d_t *candidate
)
{
    return filter == NULL || filter->allowed == NULL || filter->allowed(candidate, filter->context);
}


/*
 * Optional straight-ray clearance test for the analytical (straight-ray)
 * backends (TDCAS, PSAFUVS): false when the straight segment sensor ->
 * candidate crosses scan-verified empty space, i.e. the straight-ray
 * travel-time model cannot hold for that sensor. A zero-initialized filter
 * (clear == NULL) treats every ray as clear.
 */
typedef bool (*localization_ray_clear_fn)(
    const localization_point3d_t *sensor,
    const localization_point3d_t *candidate,
    void *context
);

typedef struct
{
    localization_ray_clear_fn clear;
    void *context;

} localization_ray_filter_t;


static inline bool localization_ray_admitted(
    const localization_ray_filter_t *filter,
    const localization_point3d_t *sensor,
    const localization_point3d_t *candidate
)
{
    return filter == NULL || filter->clear == NULL || filter->clear(sensor, candidate, filter->context);
}


/*
 * Optional travel-time model replacing distance / propagation_speed for the
 * grid-search backends that accept one: the first-arrival travel
 * time, in seconds, from `candidate` to sensor `sensor_index` at the
 * backend's reference velocity (e.g. an eikonal field computed through a
 * scan's voids). False when that sensor cannot be reached from the
 * candidate. A zero-initialized model (travel_time == NULL) keeps straight
 * rays.
 */
typedef bool (*localization_travel_time_fn)(
    size_t sensor_index,
    const localization_point3d_t *candidate,
    double *travel_time_s,
    void *context
);

typedef struct
{
    localization_travel_time_fn travel_time;
    void *context;

} localization_travel_time_model_t;


typedef struct
{
    localization_point3d_t sensor_position;
    int64_t arrival_time_us;

} localization_observation_t;


typedef struct
{
    /*
     * Must be supplied from the measurement model / calibration. There is no
     * universal propagation speed for heterogeneous rubble or structures.
     */
    double propagation_speed_mps;

    uint8_t max_iterations;
    double convergence_m;
    double damping;

} localization_config_t;


typedef struct
{
    localization_point3d_t source_position;
    int64_t emission_time_us;

    double rms_residual_us;
    uint8_t iterations;
    bool converged;

} localization_result_t;


/*
 * Returns numerical solver defaults. propagation_speed_mps is deliberately
 * zero so the caller must set a measured/modelled value before solving.
 */
localization_config_t localization_default_config(void);


/*
 * MAX-211 depth-resolution test shared by every backend: true when some
 * four of the positions span a tetrahedron whose |6 x volume| exceeds
 * 0.05 x (largest pairwise distance)^3, i.e. the layout has enough vertical
 * relief to resolve depth. A near-planar surface array returns false.
 */
bool localization_positions_resolve_depth(
    const localization_point3d_t *positions,
    size_t position_count
);


/*
 * Solve a 3D time-difference-of-arrival problem.
 *
 * Arrival times must refer to the same detected event. Sensor positions and
 * propagation speed must use meters / meters-per-second. At least four
 * non-coplanar sensors are required for a 3D solution.
 *
 * The solver estimates source position from TDOA, then derives emission time
 * from the solved distances. Detection / arrival-picking and propagation-speed
 * estimation are intentionally separate preprocessing/model responsibilities.
 */
esp_err_t localization_tdoa_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_result_t *result
);
