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


typedef struct
{
    localization_point3d_t sensor_position;
    int64_t arrival_time_us;

} localization_observation_t;


typedef struct
{
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


localization_config_t localization_default_config(void);


/*
 * Solve a 3D time-difference-of-arrival problem.
 *
 * Arrival times must refer to the same detected event. Sensor positions and
 * propagation speed must use meters / meters-per-second. At least four
 * non-coplanar sensors are required for an unambiguous 3D solution.
 *
 * The solver estimates source position from TDOA, then derives emission time
 * from the solved distances. Detection / arrival-picking is intentionally a
 * separate preprocessing responsibility.
 */
esp_err_t localization_tdoa_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_result_t *result
);
