#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "geophone_events.h"
#include "localization.h"

/*
 * MAX-135: wires geophone_events picks into localization_tdoa_solve()
 * without merging arrival picking into the solver itself. localization.c
 * stays pure TDOA math; this file is the only place that knows how to turn
 * a set of per-channel picks into localization_observation_t values.
 *
 * Per the MAX-132 project rule, propagation_speed_mps is never invented
 * here -- it is part of the caller-supplied localization_config_t, exactly
 * as localization.h already requires.
 */


typedef struct
{
    geophone_channel_pick_t pick;
    uint32_t sample_rate_hz;
    localization_point3d_t sensor_position;

} localization_event_channel_input_t;


typedef enum
{
    /* Fewer than LOCALIZATION_MIN_SENSORS_3D channels had an accepted pick;
     * localization_tdoa_solve() was never called. */
    LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS = 0,

    /* Enough channels had accepted picks, but localization_tdoa_solve()
     * itself rejected the input (degenerate/coplanar geometry, singular
     * normal equations, non-finite estimate). */
    LOCALIZATION_EVENT_SOLVER_REJECTED,

    /* The solver ran. Inspect solver_result.converged and
     * solver_result.rms_residual_us for solve quality -- a low-quality or
     * non-converged solve is still reported here, not upgraded/downgraded
     * by this bridge. */
    LOCALIZATION_EVENT_SOLVED

} localization_event_status_t;


typedef struct
{
    localization_event_status_t status;

    /* Bit i corresponds to channels[i]. Exactly one of the two masks has
     * bit i set for every i < channel_count. */
    uint32_t channel_mask_used;
    uint32_t channel_mask_rejected;
    size_t channels_used_count;

    /* Valid only when status == LOCALIZATION_EVENT_SOLVED. */
    localization_result_t solver_result;

} localization_event_result_t;


/*
 * channel_count must not exceed 32 (channel_mask_* bit width). Channels
 * whose pick.status != GEOPHONE_PICK_ACCEPTED are excluded from the solve
 * and reported in channel_mask_rejected; this is the "reject/flag rather
 * than invent" contract carried forward from MAX-134.
 */
esp_err_t localization_event_solve(
    const localization_event_channel_input_t *channels,
    size_t channel_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_event_result_t *result
);
